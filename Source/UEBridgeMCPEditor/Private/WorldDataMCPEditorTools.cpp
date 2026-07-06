#include "WorldDataMCPEditorTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataSceneBriefStore.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorBuildUtils.h"
#include "Engine/Engine.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/TextureFactory.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "ReferenceSkeleton.h"
#include "StaticMeshResources.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace WorldDataMCP
{
namespace EditorTools
{
namespace
{
	UWorld* GetEditorWorld()
	{
		if (GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
		return GWorld;
	}

	// Resolve a placed actor by editor label first, then internal name; case-sensitive then
	// case-insensitive so an agent can pass either form.
	AActor* FindActor(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
		{
			return nullptr;
		}
		AActor* CaseInsensitive = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			if (Actor->GetActorLabel() == NameOrLabel || Actor->GetName() == NameOrLabel)
			{
				return Actor;
			}
			if (!CaseInsensitive
				&& (Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase)
					|| Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase)))
			{
				CaseInsensitive = Actor;
			}
		}
		return CaseInsensitive;
	}

	// Accept either a package path (/Game/Foo/Bar) or a full object path (/Game/Foo/Bar.Bar);
	// the asset-name suffix is appended when missing so StaticLoadObject resolves it.
	FString NormalizeObjectPath(FString Path)
	{
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty() || Path.Contains(TEXT(".")))
		{
			return Path;
		}
		const FString AssetName = FPaths::GetBaseFilename(Path);
		return FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
	}

	UObject* LoadObjectFromPath(const FString& Path)
	{
		const FString Normalized = NormalizeObjectPath(Path);
		if (Normalized.IsEmpty())
		{
			return nullptr;
		}
		return StaticLoadObject(UObject::StaticClass(), nullptr, *Normalized);
	}

	const TCHAR* BlendModeName(EBlendMode Mode)
	{
		switch (Mode)
		{
		case BLEND_Opaque: return TEXT("Opaque");
		case BLEND_Masked: return TEXT("Masked");
		case BLEND_Translucent: return TEXT("Translucent");
		case BLEND_Additive: return TEXT("Additive");
		case BLEND_Modulate: return TEXT("Modulate");
		case BLEND_AlphaComposite: return TEXT("AlphaComposite");
		case BLEND_AlphaHoldout: return TEXT("AlphaHoldout");
		default: return TEXT("Unknown");
		}
	}

	// ---- Group A: console & CVars -------------------------------------------------------

	// Temporarily tees GLog so output produced while a console command runs is captured and
	// returned to the agent instead of vanishing into the editor log.
	class FCaptureOutputDevice : public FOutputDevice
	{
	public:
		FString Captured;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override
		{
			Captured += V;
			Captured += TEXT("\n");
		}
	};

	FString ExecuteConsoleCommand(const TSharedPtr<FJsonObject>& Args)
	{
		FString Command;
		if (!Args->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'command'."));
		}

		UWorld* World = GetEditorWorld();
		FCaptureOutputDevice Capture;
		if (GLog)
		{
			GLog->AddOutputDevice(&Capture);
		}
		const bool bHandled = GEngine ? GEngine->Exec(World, *Command, Capture) : false;
		if (GLog)
		{
			GLog->Flush();
			GLog->RemoveOutputDevice(&Capture);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("command"), Command);
		Result->SetBoolField(TEXT("handled"), bHandled);
		Result->SetStringField(TEXT("output"), Capture.Captured);
		return SuccessJson(Result);
	}

	FString GetConsoleVariable(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'name'."));
		}
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (!CVar)
		{
			return ErrorJson(FString::Printf(TEXT("Console variable '%s' not found."), *Name));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("value"), CVar->GetString());
		return SuccessJson(Result);
	}

	FString SetConsoleVariable(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'name'."));
		}
		FString Value;
		if (!Args->TryGetStringField(TEXT("value"), Value))
		{
			// Allow numeric values supplied as JSON numbers.
			double Number = 0.0;
			if (Args->TryGetNumberField(TEXT("value"), Number))
			{
				Value = FString::SanitizeFloat(Number);
			}
			else
			{
				return ErrorJson(TEXT("Missing 'value'."));
			}
		}
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (!CVar)
		{
			return ErrorJson(FString::Printf(TEXT("Console variable '%s' not found."), *Name));
		}
		CVar->Set(*Value, ECVF_SetByConsole);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("value"), CVar->GetString());
		return SuccessJson(Result);
	}

	FString FindConsoleVariables(const TSharedPtr<FJsonObject>& Args)
	{
		FString Pattern;
		Args->TryGetStringField(TEXT("pattern"), Pattern);

		double MaxResultsNumber = 100.0;
		Args->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
		const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsNumber), 1, 1000);

		TArray<TSharedPtr<FJsonValue>> Matches;
		int32 TotalMatched = 0;
		auto Visitor = [&Matches, &TotalMatched, MaxResults](const TCHAR* Name, IConsoleObject* Obj)
		{
			++TotalMatched;
			if (Matches.Num() >= MaxResults)
			{
				return;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Name);
			if (IConsoleVariable* AsVar = Obj->AsVariable())
			{
				Entry->SetStringField(TEXT("value"), AsVar->GetString());
			}
			else
			{
				Entry->SetBoolField(TEXT("isCommand"), true);
			}
			Matches.Add(MakeShared<FJsonValueObject>(Entry));
		};
		IConsoleManager::Get().ForEachConsoleObjectThatContains(
			FConsoleObjectVisitor::CreateLambda(Visitor), *Pattern);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("pattern"), Pattern);
		Result->SetNumberField(TEXT("matched"), TotalMatched);
		Result->SetBoolField(TEXT("truncated"), Matches.Num() < TotalMatched);
		Result->SetArrayField(TEXT("variables"), Matches);
		return SuccessJson(Result);
	}

	FString TakeHighResScreenshot(const TSharedPtr<FJsonObject>& Args)
	{
		FString ResolutionArg;
		if (!Args->TryGetStringField(TEXT("resolution"), ResolutionArg) || ResolutionArg.IsEmpty())
		{
			double Mult = 2.0;
			Args->TryGetNumberField(TEXT("multiplier"), Mult);
			ResolutionArg = FString::SanitizeFloat(FMath::Clamp(Mult, 1.0, 16.0));
		}

		UWorld* World = GetEditorWorld();
		const FString Command = FString::Printf(TEXT("HighResShot %s"), *ResolutionArg);
		const bool bHandled = GEngine ? GEngine->Exec(World, *Command, *GLog) : false;

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("command"), Command);
		Result->SetBoolField(TEXT("requested"), bHandled);
		// HighResShot writes asynchronously to the project screenshot dir on the next rendered frame.
		Result->SetStringField(TEXT("outputDir"), FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir()));
		Result->SetStringField(TEXT("note"), TEXT("The PNG is written on the next rendered frame; check outputDir for the newest file."));
		return SuccessJson(Result);
	}

	// ---- Group B: level & outliner ------------------------------------------------------

	FString CreateLevel(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(/*bSaveExistingMap*/false);
		if (!NewWorld)
		{
			return ErrorJson(TEXT("Failed to create a new blank map."));
		}

		FString PackagePath;
		bool bSaved = false;
		if (Args->TryGetStringField(TEXT("packagePath"), PackagePath) && !PackagePath.IsEmpty())
		{
			bSaved = UEditorLoadingAndSavingUtils::SaveMap(NewWorld, PackagePath);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("level"), NewWorld->GetMapName());
		Result->SetBoolField(TEXT("saved"), bSaved);
		if (!PackagePath.IsEmpty())
		{
			Result->SetStringField(TEXT("packagePath"), PackagePath);
		}
		return SuccessJson(Result);
	}

	FString LoadLevel(const TSharedPtr<FJsonObject>& Args)
	{
		FString PackagePath;
		if (!Args->TryGetStringField(TEXT("packagePath"), PackagePath) || PackagePath.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'packagePath' (e.g. /Game/Maps/MyLevel)."));
		}

		FString Filename;
		if (FPackageName::IsValidLongPackageName(PackagePath))
		{
			if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetMapPackageExtension()))
			{
				return ErrorJson(FString::Printf(TEXT("Could not resolve a map file for '%s'."), *PackagePath));
			}
		}
		else
		{
			Filename = PackagePath;
		}

		UEditorLoadingAndSavingUtils::LoadMap(Filename);

		UWorld* World = GetEditorWorld();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("requested"), PackagePath);
		Result->SetStringField(TEXT("level"), World ? World->GetMapName() : FString());
		return SuccessJson(Result);
	}

	FString SaveCurrentLevel(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		// Verification gate (the "commit" chokepoint, mirror of the scene-brief pre-condition gate):
		// refuse to persist a scene that has unverified generated changes or a failing geometric
		// verdict. confirmUnverified is the explicit, logged escape hatch — not a silent skip.
		bool bConfirmUnverified = false;
		Args->TryGetBoolField(TEXT("confirmUnverified"), bConfirmUnverified);
		if (!bConfirmUnverified)
		{
			FString GateReason;
			if (::WorldDataMCP::HasUnresolvedSceneVerdict(World->GetMapName(), GateReason))
			{
				return ErrorJson(GateReason);
			}
		}

		const bool bSaved = FEditorFileUtils::SaveLevel(World->GetCurrentLevel());
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("level"), World->GetMapName());
		Result->SetBoolField(TEXT("saved"), bSaved);
		return SuccessJson(Result);
	}

	FString SaveAllDirty(const TSharedPtr<FJsonObject>& Args)
	{
		// Verification gate (commit chokepoint) — same rule as save_current_level: don't persist an
		// unverified/failing scene unless confirmUnverified is explicitly set.
		bool bConfirmUnverified = false;
		Args->TryGetBoolField(TEXT("confirmUnverified"), bConfirmUnverified);
		if (!bConfirmUnverified)
		{
			UWorld* World = GetEditorWorld();
			FString GateReason;
			if (World && ::WorldDataMCP::HasUnresolvedSceneVerdict(World->GetMapName(), GateReason))
			{
				return ErrorJson(GateReason);
			}
		}

		// No prompts: this runs unattended for an agent. Saves both map and content packages.
		const bool bSaved = FEditorFileUtils::SaveDirtyPackages(
			/*bPromptUserToSave*/false, /*bSaveMapPackages*/true, /*bSaveContentPackages*/true);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("saved"), bSaved);
		return SuccessJson(Result);
	}

	FString BuildLighting(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		// bAllowLightingDialog=false so the build proceeds without blocking on a modal dialog.
		const bool bStarted = FEditorBuildUtils::EditorBuild(World, FBuildOptions::BuildLighting, /*bAllowLightingDialog*/false);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("level"), World->GetMapName());
		Result->SetBoolField(TEXT("started"), bStarted);
		Result->SetStringField(TEXT("note"), TEXT("Lighting build runs asynchronously; check the log/Build progress."));
		return SuccessJson(Result);
	}

	FString SetActorFolder(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		FString FolderPath;
		Args->TryGetStringField(TEXT("folderPath"), FolderPath);
		if (Name.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'name'."));
		}
		UWorld* World = GetEditorWorld();
		AActor* Actor = FindActor(World, Name);
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *Name));
		}
		Actor->SetFolderPath(FName(*FolderPath));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("folderPath"), Actor->GetFolderPath().ToString());
		return SuccessJson(Result);
	}

	FString RenameActor(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		FString NewLabel;
		Args->TryGetStringField(TEXT("newLabel"), NewLabel);
		if (Name.IsEmpty() || NewLabel.IsEmpty())
		{
			return ErrorJson(TEXT("Both 'name' and 'newLabel' are required."));
		}
		UWorld* World = GetEditorWorld();
		AActor* Actor = FindActor(World, Name);
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *Name));
		}
		Actor->SetActorLabel(NewLabel);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		return SuccessJson(Result);
	}

	FString FocusActor(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'name'."));
		}
		UWorld* World = GetEditorWorld();
		AActor* Actor = FindActor(World, Name);
		if (!Actor || !GEditor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *Name));
		}
		GEditor->SelectNone(/*bNoteSelectionChange*/false, /*bDeselectBSPSurfs*/true);
		GEditor->SelectActor(Actor, /*bInSelected*/true, /*bNotify*/true);
		GEditor->MoveViewportCamerasToActor(*Actor, /*bActiveViewportOnly*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		return SuccessJson(Result);
	}

	// ---- Group C: asset inspection (read-only) ------------------------------------------

	FString InspectStaticMesh(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UStaticMesh* Mesh = Cast<UStaticMesh>(LoadObjectFromPath(AssetPath));
		if (!Mesh)
		{
			return ErrorJson(FString::Printf(TEXT("StaticMesh '%s' not found."), *AssetPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Mesh->GetPathName());

		TArray<TSharedPtr<FJsonValue>> Lods;
		if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
		{
			for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); ++LODIndex)
			{
				const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("lod"), LODIndex);
				Entry->SetNumberField(TEXT("triangles"), LOD.GetNumTriangles());
				Entry->SetNumberField(TEXT("vertices"), LOD.GetNumVertices());
				Entry->SetNumberField(TEXT("sections"), LOD.Sections.Num());
				Lods.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		Result->SetNumberField(TEXT("numLODs"), Lods.Num());
		Result->SetArrayField(TEXT("lods"), Lods);

		TArray<TSharedPtr<FJsonValue>> Materials;
		const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
		for (const FStaticMaterial& Slot : StaticMaterials)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("slot"), Slot.MaterialSlotName.ToString());
			Entry->SetStringField(TEXT("material"), Slot.MaterialInterface ? Slot.MaterialInterface->GetPathName() : FString());
			Materials.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("materials"), Materials);

		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		TSharedRef<FJsonObject> Extent = MakeShared<FJsonObject>();
		Extent->SetNumberField(TEXT("x"), Bounds.BoxExtent.X);
		Extent->SetNumberField(TEXT("y"), Bounds.BoxExtent.Y);
		Extent->SetNumberField(TEXT("z"), Bounds.BoxExtent.Z);
		Result->SetObjectField(TEXT("boundsExtent"), Extent);
		Result->SetBoolField(TEXT("nanite"), Mesh->IsNaniteEnabled());
		if (UBodySetup* BodySetup = Mesh->GetBodySetup())
		{
			TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
			Collision->SetNumberField(TEXT("convexElems"), BodySetup->AggGeom.ConvexElems.Num());
			Collision->SetNumberField(TEXT("boxElems"), BodySetup->AggGeom.BoxElems.Num());
			Collision->SetNumberField(TEXT("sphereElems"), BodySetup->AggGeom.SphereElems.Num());
			Collision->SetNumberField(TEXT("capsuleElems"), BodySetup->AggGeom.SphylElems.Num());
			Result->SetObjectField(TEXT("collision"), Collision);
		}
		return SuccessJson(Result);
	}

	FString InspectSkeletalMesh(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(LoadObjectFromPath(AssetPath));
		if (!Mesh)
		{
			return ErrorJson(FString::Printf(TEXT("SkeletalMesh '%s' not found."), *AssetPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Mesh->GetPathName());
		Result->SetNumberField(TEXT("numLODs"), Mesh->GetLODNum());
		Result->SetNumberField(TEXT("boneCount"), Mesh->GetRefSkeleton().GetNum());

		TArray<TSharedPtr<FJsonValue>> Materials;
		for (const FSkeletalMaterial& Slot : Mesh->GetMaterials())
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("slot"), Slot.MaterialSlotName.ToString());
			Entry->SetStringField(TEXT("material"), Slot.MaterialInterface ? Slot.MaterialInterface->GetPathName() : FString());
			Materials.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("materials"), Materials);

		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		TSharedRef<FJsonObject> Extent = MakeShared<FJsonObject>();
		Extent->SetNumberField(TEXT("x"), Bounds.BoxExtent.X);
		Extent->SetNumberField(TEXT("y"), Bounds.BoxExtent.Y);
		Extent->SetNumberField(TEXT("z"), Bounds.BoxExtent.Z);
		Result->SetObjectField(TEXT("boundsExtent"), Extent);
		return SuccessJson(Result);
	}

	void AppendMaterialParameters(const TSharedRef<FJsonObject>& Result, UMaterialInterface* Mat)
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;

		TArray<TSharedPtr<FJsonValue>> Scalars;
		Mat->GetAllScalarParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			float Value = 0.0f;
			Mat->GetScalarParameterValue(Info, Value);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Info.Name.ToString());
			Entry->SetNumberField(TEXT("value"), Value);
			Scalars.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("scalarParameters"), Scalars);

		Infos.Reset();
		Ids.Reset();
		TArray<TSharedPtr<FJsonValue>> Vectors;
		Mat->GetAllVectorParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			FLinearColor Value = FLinearColor::Black;
			Mat->GetVectorParameterValue(Info, Value);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Info.Name.ToString());
			Entry->SetNumberField(TEXT("r"), Value.R);
			Entry->SetNumberField(TEXT("g"), Value.G);
			Entry->SetNumberField(TEXT("b"), Value.B);
			Entry->SetNumberField(TEXT("a"), Value.A);
			Vectors.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("vectorParameters"), Vectors);

		Infos.Reset();
		Ids.Reset();
		TArray<TSharedPtr<FJsonValue>> Textures;
		Mat->GetAllTextureParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			UTexture* Value = nullptr;
			Mat->GetTextureParameterValue(Info, Value);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Info.Name.ToString());
			Entry->SetStringField(TEXT("texture"), Value ? Value->GetPathName() : FString());
			Textures.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("textureParameters"), Textures);
	}

	FString InspectMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UMaterialInterface* Mat = Cast<UMaterialInterface>(LoadObjectFromPath(AssetPath));
		if (!Mat)
		{
			return ErrorJson(FString::Printf(TEXT("Material '%s' not found."), *AssetPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Mat->GetPathName());
		if (const UMaterial* BaseMaterial = Mat->GetMaterial())
		{
			Result->SetStringField(TEXT("baseMaterial"), BaseMaterial->GetPathName());
		}
		Result->SetStringField(TEXT("blendMode"), BlendModeName(Mat->GetBlendMode()));
		Result->SetBoolField(TEXT("twoSided"), Mat->IsTwoSided());
		AppendMaterialParameters(Result, Mat);
		return SuccessJson(Result);
	}

	FString InspectTexture(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UTexture2D* Tex = Cast<UTexture2D>(LoadObjectFromPath(AssetPath));
		if (!Tex)
		{
			return ErrorJson(FString::Printf(TEXT("Texture2D '%s' not found."), *AssetPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Tex->GetPathName());
		Result->SetNumberField(TEXT("width"), Tex->GetSizeX());
		Result->SetNumberField(TEXT("height"), Tex->GetSizeY());
		Result->SetNumberField(TEXT("numMips"), Tex->GetNumMips());
		Result->SetBoolField(TEXT("sRGB"), Tex->SRGB != 0);
		Result->SetNumberField(TEXT("compressionSettings"), static_cast<int32>(Tex->CompressionSettings.GetValue()));
		Result->SetNumberField(TEXT("lodGroup"), static_cast<int32>(Tex->LODGroup.GetValue()));
		const FString PixelFormat = GetPixelFormatString(Tex->GetPixelFormat());
		Result->SetStringField(TEXT("pixelFormat"), PixelFormat);
		return SuccessJson(Result);
	}

	// ---- Group D + E: material editing & editor state -----------------------------------

	bool MakeAssetPackage(const FString& DestPackagePath, UPackage*& OutPackage, FString& OutAssetName, FString& OutError)
	{
		FString PackagePath = DestPackagePath;
		PackagePath.TrimStartAndEndInline();
		if (PackagePath.IsEmpty() || !PackagePath.StartsWith(TEXT("/")))
		{
			OutError = TEXT("destPath must be a content path like /Game/Materials/MI_Foo.");
			return false;
		}
		// Strip any object suffix (.Foo) — we want the package path.
		int32 DotIndex = INDEX_NONE;
		if (PackagePath.FindChar(TEXT('.'), DotIndex))
		{
			PackagePath.LeftInline(DotIndex);
		}
		OutAssetName = FPackageName::GetShortName(PackagePath);
		if (OutAssetName.IsEmpty())
		{
			OutError = TEXT("Could not derive an asset name from destPath.");
			return false;
		}
		OutPackage = CreatePackage(*PackagePath);
		if (!OutPackage)
		{
			OutError = FString::Printf(TEXT("Failed to create package '%s'."), *PackagePath);
			return false;
		}
		return true;
	}

	FString CreateMaterialInstance(const TSharedPtr<FJsonObject>& Args)
	{
		FString ParentPath;
		Args->TryGetStringField(TEXT("parentPath"), ParentPath);
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		if (ParentPath.IsEmpty() || DestPath.IsEmpty())
		{
			return ErrorJson(TEXT("Both 'parentPath' and 'destPath' are required."));
		}
		UMaterialInterface* Parent = Cast<UMaterialInterface>(LoadObjectFromPath(ParentPath));
		if (!Parent)
		{
			return ErrorJson(FString::Printf(TEXT("Parent material '%s' not found."), *ParentPath));
		}

		UPackage* Package = nullptr;
		FString AssetName;
		FString Error;
		if (!MakeAssetPackage(DestPath, Package, AssetName, Error))
		{
			return ErrorJson(Error);
		}

		UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(
			Package, *AssetName, RF_Public | RF_Standalone);
		if (!MIC)
		{
			return ErrorJson(TEXT("Failed to create material instance object."));
		}
		MIC->SetParentEditorOnly(Parent);
		MIC->PostEditChange();
		FAssetRegistryModule::AssetCreated(MIC);
		Package->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), MIC->GetPathName());
		Result->SetStringField(TEXT("parent"), Parent->GetPathName());
		Result->SetStringField(TEXT("note"), TEXT("Created in memory and marked dirty; call save_asset to persist."));
		return SuccessJson(Result);
	}

	FString SetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		FString ParameterName;
		Args->TryGetStringField(TEXT("parameter"), ParameterName);
		if (AssetPath.IsEmpty() || ParameterName.IsEmpty())
		{
			return ErrorJson(TEXT("Both 'assetPath' and 'parameter' are required."));
		}
		UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(LoadObjectFromPath(AssetPath));
		if (!MIC)
		{
			return ErrorJson(FString::Printf(TEXT("Material instance '%s' not found."), *AssetPath));
		}

		const FMaterialParameterInfo Info(*ParameterName);
		FString AppliedType;

		double ScalarValue = 0.0;
		const TArray<TSharedPtr<FJsonValue>>* VectorArray = nullptr;
		FString TexturePath;
		if (Args->TryGetNumberField(TEXT("scalar"), ScalarValue))
		{
			MIC->SetScalarParameterValueEditorOnly(Info, static_cast<float>(ScalarValue));
			AppliedType = TEXT("scalar");
		}
		else if (Args->TryGetArrayField(TEXT("vector"), VectorArray) && VectorArray)
		{
			FLinearColor Color = FLinearColor::Black;
			const TArray<TSharedPtr<FJsonValue>>& V = *VectorArray;
			if (V.Num() > 0) { Color.R = static_cast<float>(V[0]->AsNumber()); }
			if (V.Num() > 1) { Color.G = static_cast<float>(V[1]->AsNumber()); }
			if (V.Num() > 2) { Color.B = static_cast<float>(V[2]->AsNumber()); }
			Color.A = V.Num() > 3 ? static_cast<float>(V[3]->AsNumber()) : 1.0f;
			MIC->SetVectorParameterValueEditorOnly(Info, Color);
			AppliedType = TEXT("vector");
		}
		else if (Args->TryGetStringField(TEXT("texture"), TexturePath))
		{
			UTexture* Texture = Cast<UTexture>(LoadObjectFromPath(TexturePath));
			if (!Texture)
			{
				return ErrorJson(FString::Printf(TEXT("Texture '%s' not found."), *TexturePath));
			}
			MIC->SetTextureParameterValueEditorOnly(Info, Texture);
			AppliedType = TEXT("texture");
		}
		else
		{
			return ErrorJson(TEXT("Provide one of 'scalar' (number), 'vector' ([r,g,b,a]), or 'texture' (asset path)."));
		}

		MIC->PostEditChange();
		MIC->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), MIC->GetPathName());
		Result->SetStringField(TEXT("parameter"), ParameterName);
		Result->SetStringField(TEXT("type"), AppliedType);
		Result->SetStringField(TEXT("note"), TEXT("Marked dirty; call save_asset to persist."));
		return SuccessJson(Result);
	}

	FString ImportTexture(const TSharedPtr<FJsonObject>& Args)
	{
		FString SourceFile;
		Args->TryGetStringField(TEXT("sourceFile"), SourceFile);
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		if (SourceFile.IsEmpty() || DestPath.IsEmpty())
		{
			return ErrorJson(TEXT("Both 'sourceFile' (disk path) and 'destPath' (content path) are required."));
		}
		if (!FPaths::FileExists(SourceFile))
		{
			return ErrorJson(FString::Printf(TEXT("Source file '%s' does not exist."), *SourceFile));
		}

		TArray<uint8> FileData;
		if (!FFileHelper::LoadFileToArray(FileData, *SourceFile) || FileData.Num() == 0)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to read source file '%s'."), *SourceFile));
		}

		UPackage* Package = nullptr;
		FString AssetName;
		FString Error;
		if (!MakeAssetPackage(DestPath, Package, AssetName, Error))
		{
			return ErrorJson(Error);
		}

		UTextureFactory* Factory = NewObject<UTextureFactory>();
		Factory->SuppressImportOverwriteDialog();

		const FString Extension = FPaths::GetExtension(SourceFile);
		const uint8* Buffer = FileData.GetData();
		const uint8* BufferEnd = Buffer + FileData.Num();
		UObject* Created = Factory->FactoryCreateBinary(
			UTexture2D::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone,
			nullptr, *Extension, Buffer, BufferEnd, GWarn);

		UTexture2D* Texture = Cast<UTexture2D>(Created);
		if (!Texture)
		{
			return ErrorJson(FString::Printf(TEXT("Texture factory could not import '%s'."), *SourceFile));
		}

		// Optional post-import tweaks an agent commonly needs.
		bool bSRGB = true;
		if (Args->TryGetBoolField(TEXT("sRGB"), bSRGB))
		{
			Texture->SRGB = bSRGB;
		}
		Texture->PostEditChange();
		FAssetRegistryModule::AssetCreated(Texture);
		Package->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Texture->GetPathName());
		Result->SetNumberField(TEXT("width"), Texture->GetSizeX());
		Result->SetNumberField(TEXT("height"), Texture->GetSizeY());
		Result->SetStringField(TEXT("note"), TEXT("Imported in memory and marked dirty; call save_asset to persist."));
		return SuccessJson(Result);
	}

	FString UndoTransaction(const TSharedPtr<FJsonObject>& Args)
	{
		if (!GEditor)
		{
			return ErrorJson(TEXT("Editor is not available."));
		}
		const bool bOk = GEditor->UndoTransaction();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("undone"), bOk);
		return SuccessJson(Result);
	}

	FString RedoTransaction(const TSharedPtr<FJsonObject>& Args)
	{
		if (!GEditor)
		{
			return ErrorJson(TEXT("Editor is not available."));
		}
		const bool bOk = GEditor->RedoTransaction();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("redone"), bOk);
		return SuccessJson(Result);
	}

	FString SaveAsset(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		if (AssetPath.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'assetPath'."));
		}
		UObject* Object = LoadObjectFromPath(AssetPath);
		if (!Object)
		{
			return ErrorJson(FString::Printf(TEXT("Asset '%s' not found."), *AssetPath));
		}
		UPackage* Package = Object->GetOutermost();
		if (!Package)
		{
			return ErrorJson(TEXT("Asset has no package."));
		}

		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
		{
			return ErrorJson(FString::Printf(TEXT("Could not resolve a filename for package '%s'."), *Package->GetName()));
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Object->GetPathName());
		Result->SetStringField(TEXT("file"), Filename);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"execute_console_command","description":"Run a UE console command in the editor and capture log output produced while it runs (e.g. 'stat unit', 'r.ScreenPercentage 50', 'BUILDPATHS').","inputSchema":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]},"annotations":{"title":"Execute Console Command","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"get_console_variable","description":"Read the current value of a console variable (CVar) by name.","inputSchema":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]},"annotations":{"title":"Get Console Variable","readOnlyHint":true,"openWorldHint":false}},
{"name":"set_console_variable","description":"Set a console variable (CVar) by name. Value may be a string or number.","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"value":{"oneOf":[{"type":"string"},{"type":"number"}],"description":"String or number."}},"required":["name","value"]},"annotations":{"title":"Set Console Variable","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"find_console_variables","description":"Enumerate console objects (CVars and commands) whose name contains a substring.","inputSchema":{"type":"object","properties":{"pattern":{"type":"string","description":"Case-insensitive substring; empty lists everything."},"maxResults":{"type":"number","description":"Default 100, capped at 1000."}}},"annotations":{"title":"Find Console Variables","readOnlyHint":true,"openWorldHint":false}},
{"name":"take_high_res_screenshot","description":"Trigger UE's HighResShot to write a high-resolution PNG of the active viewport to the project screenshot directory.","inputSchema":{"type":"object","properties":{"resolution":{"type":"string","description":"e.g. '3840x2160'. Overrides multiplier."},"multiplier":{"type":"number","description":"Resolution multiplier 1-16 when resolution is omitted. Default 2."}}},"annotations":{"title":"High-Res Screenshot","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"create_level","description":"Create a new blank level. Optionally save it to a content path.","inputSchema":{"type":"object","properties":{"packagePath":{"type":"string","description":"Optional content path to save the new level, e.g. /Game/Maps/MyLevel."}}},"annotations":{"title":"Create Level","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"load_level","description":"Open a level by content path (e.g. /Game/Maps/MyLevel). Discards unsaved changes in the current level.","inputSchema":{"type":"object","properties":{"packagePath":{"type":"string"}},"required":["packagePath"]},"annotations":{"title":"Load Level","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"save_current_level","description":"Save the currently open level. GATED: refuses if the scene has unverified generated changes or its last geometric verdict was FAIL — run verify_scene first, or pass confirmUnverified:true to persist work-in-progress anyway.","inputSchema":{"type":"object","properties":{"confirmUnverified":{"type":"boolean","description":"Explicitly acknowledge saving an unverified/failing scene (bypasses the verification gate). Default false."}}},"annotations":{"title":"Save Current Level","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"save_all_dirty","description":"Save all dirty map and content packages without prompting. GATED: refuses if the current scene has unverified generated changes or its last geometric verdict was FAIL — run verify_scene first, or pass confirmUnverified:true to override.","inputSchema":{"type":"object","properties":{"confirmUnverified":{"type":"boolean","description":"Explicitly acknowledge saving an unverified/failing scene (bypasses the verification gate). Default false."}}},"annotations":{"title":"Save All Dirty","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"build_lighting","description":"Start a static lighting build for the current editor world (runs asynchronously).","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Build Lighting","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_actor_folder","description":"Move an actor into a World Outliner folder path (e.g. 'Lighting/Sky'). Empty path moves it to the root.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or label."},"folderPath":{"type":"string"}},"required":["name"]},"annotations":{"title":"Set Actor Folder","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"rename_actor","description":"Change an actor's editor label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or current label."},"newLabel":{"type":"string"}},"required":["name","newLabel"]},"annotations":{"title":"Rename Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"focus_actor","description":"Select an actor and frame the editor viewport camera on it.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or label."}},"required":["name"]},"annotations":{"title":"Focus Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
)JSON")
		TEXT(R"JSON({"name":"inspect_static_mesh","description":"Inspect a StaticMesh asset: per-LOD triangle/vertex/section counts, material slots, bounds, Nanite flag, and collision primitive counts.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Inspect Static Mesh","readOnlyHint":true,"openWorldHint":false}},
{"name":"inspect_skeletal_mesh","description":"Inspect a SkeletalMesh asset: LOD count, bone count, material slots, and bounds.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Inspect Skeletal Mesh","readOnlyHint":true,"openWorldHint":false}},
{"name":"inspect_material","description":"Inspect a Material or MaterialInstance: base material, blend mode, two-sided flag, and scalar/vector/texture parameters with current values.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Inspect Material","readOnlyHint":true,"openWorldHint":false}},
{"name":"inspect_texture","description":"Inspect a Texture2D: dimensions, mip count, sRGB, compression settings, LOD group, and pixel format.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Inspect Texture","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_material_instance","description":"Create a Material Instance Constant under destPath with the given parent material. Created in memory and marked dirty; call save_asset to persist.","inputSchema":{"type":"object","properties":{"parentPath":{"type":"string","description":"Parent material/instance asset path."},"destPath":{"type":"string","description":"Content path for the new instance, e.g. /Game/Materials/MI_Foo."}},"required":["parentPath","destPath"]},"annotations":{"title":"Create Material Instance","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_material_instance_parameter","description":"Override a parameter on a Material Instance Constant. Provide exactly one of scalar/vector/texture. Marks the asset dirty.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"parameter":{"type":"string"},"scalar":{"type":"number"},"vector":{"type":"array","items":{"type":"number"},"description":"[r,g,b,a] (a defaults to 1)."},"texture":{"type":"string","description":"Texture asset path."}},"required":["assetPath","parameter"]},"annotations":{"title":"Set Material Instance Parameter","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"import_texture","description":"Import an image file (PNG/JPG/TGA/EXR/BMP/HDR) from disk into a Texture2D asset at destPath. Created in memory and marked dirty; call save_asset to persist.","inputSchema":{"type":"object","properties":{"sourceFile":{"type":"string","description":"Absolute path to the image file on disk."},"destPath":{"type":"string","description":"Content path for the new texture, e.g. /Game/Textures/T_Foo."},"sRGB":{"type":"boolean","description":"Optional sRGB override."}},"required":["sourceFile","destPath"]},"annotations":{"title":"Import Texture","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"undo","description":"Step the editor undo history backward by one transaction.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Undo","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"redo","description":"Step the editor undo history forward by one transaction.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Redo","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"save_asset","description":"Save a single asset's package to disk by content path.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Save Asset","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("execute_console_command")) { OutResult = ExecuteConsoleCommand(Args); return true; }
	if (ToolName == TEXT("get_console_variable")) { OutResult = GetConsoleVariable(Args); return true; }
	if (ToolName == TEXT("set_console_variable")) { OutResult = SetConsoleVariable(Args); return true; }
	if (ToolName == TEXT("find_console_variables")) { OutResult = FindConsoleVariables(Args); return true; }
	if (ToolName == TEXT("take_high_res_screenshot")) { OutResult = TakeHighResScreenshot(Args); return true; }

	if (ToolName == TEXT("create_level")) { OutResult = CreateLevel(Args); return true; }
	if (ToolName == TEXT("load_level")) { OutResult = LoadLevel(Args); return true; }
	if (ToolName == TEXT("save_current_level")) { OutResult = SaveCurrentLevel(Args); return true; }
	if (ToolName == TEXT("save_all_dirty")) { OutResult = SaveAllDirty(Args); return true; }
	if (ToolName == TEXT("build_lighting")) { OutResult = BuildLighting(Args); return true; }
	if (ToolName == TEXT("set_actor_folder")) { OutResult = SetActorFolder(Args); return true; }
	if (ToolName == TEXT("rename_actor")) { OutResult = RenameActor(Args); return true; }
	if (ToolName == TEXT("focus_actor")) { OutResult = FocusActor(Args); return true; }

	if (ToolName == TEXT("inspect_static_mesh")) { OutResult = InspectStaticMesh(Args); return true; }
	if (ToolName == TEXT("inspect_skeletal_mesh")) { OutResult = InspectSkeletalMesh(Args); return true; }
	if (ToolName == TEXT("inspect_material")) { OutResult = InspectMaterial(Args); return true; }
	if (ToolName == TEXT("inspect_texture")) { OutResult = InspectTexture(Args); return true; }

	if (ToolName == TEXT("create_material_instance")) { OutResult = CreateMaterialInstance(Args); return true; }
	if (ToolName == TEXT("set_material_instance_parameter")) { OutResult = SetMaterialInstanceParameter(Args); return true; }
	if (ToolName == TEXT("import_texture")) { OutResult = ImportTexture(Args); return true; }
	if (ToolName == TEXT("undo")) { OutResult = UndoTransaction(Args); return true; }
	if (ToolName == TEXT("redo")) { OutResult = RedoTransaction(Args); return true; }
	if (ToolName == TEXT("save_asset")) { OutResult = SaveAsset(Args); return true; }

	return false;
}
}
}
