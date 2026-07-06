#include "UEBridgeMCPExtractedTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataMCPServer.h"
#include "WorldDataMCPTools.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

namespace WorldDataMCP
{
namespace ExtractedTools
{
namespace
{
	constexpr int32 MaxFileReadBytes = 1024 * 1024;
	constexpr int32 MaxFileWriteChars = 1024 * 1024;
	constexpr int32 MaxPythonLogEntries = 200;
	constexpr int32 MaxPythonOutputChars = 8192;
	constexpr int32 MaxPythonResultChars = 65536;
	constexpr int32 MaxSourceFiles = 400;
	constexpr int32 MaxResourceRows = 300;
	constexpr int64 MaxRecipeJsonReadBytes = 2 * 1024 * 1024;

	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		return JsonObjectToString(Object);
	}

	TSharedPtr<FJsonObject> LoadJsonObjectFile(const FString& Path)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *Path))
		{
			return nullptr;
		}
		return ParseJsonObject(Content);
	}

	FString MakeProjectRelative(FString Path)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		if (FPaths::MakePathRelativeTo(Path, *ProjectDir))
		{
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			return Path;
		}
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Path;
	}

	bool ResolveProjectFilePath(const FString& InputPath, FString& OutPath, FString& OutError)
	{
		if (InputPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("file_path is required.");
			return false;
		}

		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::CollapseRelativeDirectories(ProjectDir);
		FPaths::NormalizeDirectoryName(ProjectDir);

		FString Candidate = FPaths::IsRelative(InputPath)
			? FPaths::Combine(ProjectDir, InputPath)
			: InputPath;
		Candidate = FPaths::ConvertRelativePathToFull(Candidate);
		FPaths::CollapseRelativeDirectories(Candidate);
		FPaths::NormalizeFilename(Candidate);

		const bool bInProject =
			Candidate.Equals(ProjectDir, ESearchCase::IgnoreCase)
			|| Candidate.StartsWith(ProjectDir + TEXT("/"), ESearchCase::IgnoreCase);
		if (!bInProject)
		{
			OutError = FString::Printf(TEXT("Path is outside the project directory: %s"), *InputPath);
			return false;
		}

		OutPath = Candidate;
		return true;
	}

	bool WriteStringAtomically(const FString& Content, const FString& FullPath)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullPath), true);

		const FString TmpPath = FullPath + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Content, *TmpPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return false;
		}

		if (!IFileManager::Get().Move(*FullPath, *TmpPath, true, true))
		{
			IFileManager::Get().Delete(*TmpPath);
			return false;
		}
		return true;
	}

	bool HasAllowedTextFileExtension(const FString& FullPath)
	{
		const FString Extension = FPaths::GetExtension(FullPath, true).ToLower();
		static const TSet<FString> AllowedExtensions = {
			TEXT(".bat"),
			TEXT(".c"),
			TEXT(".cmd"),
			TEXT(".cpp"),
			TEXT(".cs"),
			TEXT(".css"),
			TEXT(".csv"),
			TEXT(".h"),
			TEXT(".hpp"),
			TEXT(".html"),
			TEXT(".ini"),
			TEXT(".js"),
			TEXT(".json"),
			TEXT(".jsx"),
			TEXT(".md"),
			TEXT(".ps1"),
			TEXT(".py"),
			TEXT(".sh"),
			TEXT(".toml"),
			TEXT(".ts"),
			TEXT(".tsx"),
			TEXT(".tsv"),
			TEXT(".txt"),
			TEXT(".uplugin"),
			TEXT(".uproject"),
			TEXT(".usf"),
			TEXT(".ush"),
			TEXT(".xml"),
			TEXT(".yaml"),
			TEXT(".yml")
		};
		return AllowedExtensions.Contains(Extension);
	}

	bool ValidateProjectTextFilePath(const FString& FullPath, const FString& Operation, FString& OutError)
	{
		FString Relative = MakeProjectRelative(FullPath).ToLower();
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Relative.IsEmpty() || Relative == TEXT("."))
		{
			OutError = FString::Printf(TEXT("Refusing to %s the project root."), *Operation);
			return false;
		}

		static const TSet<FString> RestrictedExactPaths = {
			TEXT(".mcp.json"),
			TEXT(".cursor/mcp.json"),
			TEXT("saved/uebridgemcp/config.json"),
			TEXT("saved/uebridgemcp/mcp.json")
		};
		if (RestrictedExactPaths.Contains(Relative))
		{
			OutError = FString::Printf(TEXT("Refusing to %s MCP connection/config file: %s"), *Operation, *MakeProjectRelative(FullPath));
			return false;
		}

		static const TArray<FString> RestrictedPrefixes = {
			TEXT(".git/"),
			TEXT("binaries/"),
			TEXT("deriveddatacache/"),
			TEXT("intermediate/"),
			TEXT("saved/logs/"),
			TEXT("saved/uebridgemcp/")
		};
		for (const FString& Prefix : RestrictedPrefixes)
		{
			if (Relative.StartsWith(Prefix))
			{
				OutError = FString::Printf(TEXT("Refusing to %s restricted project path: %s"), *Operation, *MakeProjectRelative(FullPath));
				return false;
			}
		}

		if (!HasAllowedTextFileExtension(FullPath))
		{
			OutError = FString::Printf(TEXT("Refusing to %s non-text or unsupported file type: %s"), *Operation, *MakeProjectRelative(FullPath));
			return false;
		}

		return true;
	}

	double GetNumberField(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, double DefaultValue)
	{
		double Value = DefaultValue;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Value);
		}
		return Value;
	}

	FString GetStringField(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, const FString& DefaultValue = TEXT(""))
	{
		FString Value = DefaultValue;
		if (Args.IsValid())
		{
			Args->TryGetStringField(FieldName, Value);
		}
		return Value;
	}

	bool GetBoolField(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, bool DefaultValue = false)
	{
		bool Value = DefaultValue;
		if (Args.IsValid())
		{
			Args->TryGetBoolField(FieldName, Value);
		}
		return Value;
	}

	void AddStringArray(TSharedRef<FJsonObject> Object, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	void AddResource(TArray<TSharedPtr<FJsonValue>>& Resources, const FString& Uri, const FString& Name, const FString& Description)
	{
		TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), Uri);
		Resource->SetStringField(TEXT("name"), Name);
		Resource->SetStringField(TEXT("description"), Description);
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	TSharedRef<FJsonObject> MakeVectorObject(const FVector& Vector)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Vector.X);
		Object->SetNumberField(TEXT("y"), Vector.Y);
		Object->SetNumberField(TEXT("z"), Vector.Z);
		return Object;
	}

	TSharedRef<FJsonObject> MakeRotatorObject(const FRotator& Rotator)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("pitch"), Rotator.Pitch);
		Object->SetNumberField(TEXT("yaw"), Rotator.Yaw);
		Object->SetNumberField(TEXT("roll"), Rotator.Roll);
		return Object;
	}

	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : GWorld;
	}

	FString NormalizeAssetObjectPath(FString Path)
	{
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty() || Path.Contains(TEXT(".")))
		{
			return Path;
		}

		const FString AssetName = FPaths::GetBaseFilename(Path);
		return FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
	}

	bool ResolveAssetDataByPath(const FString& InputPath, FAssetData& OutAssetData, FString& OutError)
	{
		const FString Path = InputPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			OutError = TEXT("assetPath or path is required.");
			return false;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		OutAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NormalizeAssetObjectPath(Path)));
		if (!OutAssetData.IsValid())
		{
			FString PackageName = Path;
			if (PackageName.Contains(TEXT(".")))
			{
				PackageName.Split(TEXT("."), &PackageName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			}

			TArray<FAssetData> PackageAssets;
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), PackageAssets);
			if (PackageAssets.Num() > 0)
			{
				OutAssetData = PackageAssets[0];
			}
		}

		if (!OutAssetData.IsValid())
		{
			OutError = FString::Printf(TEXT("Asset not found: %s"), *Path);
			return false;
		}

		return true;
	}

	TSharedRef<FJsonObject> MakeAssetObject(const FAssetData& AssetData)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Object->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		Object->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
		Object->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
		return Object;
	}

	TSharedRef<FJsonObject> MakePackageReferenceObject(IAssetRegistry& AssetRegistry, FName PackageName)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("packageName"), PackageName.ToString());

		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(PackageName, PackageAssets);
		if (PackageAssets.Num() > 0)
		{
			Object->SetObjectField(TEXT("asset"), MakeAssetObject(PackageAssets[0]));
		}
		return Object;
	}

	TSharedRef<FJsonObject> MakeCompactActorObject(AActor* Actor)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		if (!IsValid(Actor))
		{
			return Object;
		}

		Object->SetStringField(TEXT("name"), Actor->GetName());
		Object->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Object->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : TEXT("Unknown"));
		Object->SetStringField(TEXT("path"), Actor->GetPathName());
		Object->SetObjectField(TEXT("location"), MakeVectorObject(Actor->GetActorLocation()));
		Object->SetBoolField(TEXT("selected"), Actor->IsSelected());
		if (const USceneComponent* Root = Actor->GetRootComponent())
		{
			Object->SetStringField(TEXT("rootComponent"), Root->GetClass() ? Root->GetClass()->GetName() : TEXT("Unknown"));
		}
		return Object;
	}

	void CollectMatchingActors(const FString& Query, int32 Limit, TArray<TSharedPtr<FJsonValue>>& OutActors, int32& OutMatchedCount)
	{
		OutMatchedCount = 0;
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return;
		}

		const FString LowerQuery = Query.ToLower();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}

			const FString Probe = FString::Printf(TEXT("%s %s %s %s"),
				*Actor->GetName(),
				*Actor->GetActorLabel(),
				Actor->GetClass() ? *Actor->GetClass()->GetName() : TEXT(""),
				*Actor->GetPathName()).ToLower();

			if (!LowerQuery.IsEmpty() && !Probe.Contains(LowerQuery))
			{
				continue;
			}

			++OutMatchedCount;
			if (OutActors.Num() < Limit)
			{
				OutActors.Add(MakeShared<FJsonValueObject>(MakeCompactActorObject(Actor)));
			}
		}
	}

	void CollectMatchingSourceFiles(const FString& Query, int32 Limit, TArray<TSharedPtr<FJsonValue>>& OutFiles, int32& OutMatchedCount)
	{
		OutMatchedCount = 0;
		const FString LowerQuery = Query.ToLower();
		TArray<FString> Roots;
		Roots.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"))));
		Roots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir()));

		for (const FString& Root : Roots)
		{
			if (!IFileManager::Get().DirectoryExists(*Root))
			{
				continue;
			}

			for (const TCHAR* Pattern : { TEXT("*.h"), TEXT("*.cpp"), TEXT("*.cs"), TEXT("*.uplugin") })
			{
				TArray<FString> Files;
				IFileManager::Get().FindFilesRecursive(Files, *Root, Pattern, true, false);
				Files.Sort();
				for (const FString& File : Files)
				{
					const FString Relative = MakeProjectRelative(File);
					if (!LowerQuery.IsEmpty() && !Relative.ToLower().Contains(LowerQuery))
					{
						continue;
					}

					++OutMatchedCount;
					if (OutFiles.Num() < Limit)
					{
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("path"), Relative);
						Row->SetStringField(TEXT("extension"), FPaths::GetExtension(File));
						OutFiles.Add(MakeShared<FJsonValueObject>(Row));
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> ParseToolJson(const FString& Text)
	{
		return ParseJsonObject(Text);
	}

	FString GetProjectPluginsResource()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);

		TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
		Plugins.Sort([](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B)
		{
			return A->GetName() < B->GetName();
		});

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 Index = 0; Index < Plugins.Num() && Index < MaxResourceRows; ++Index)
		{
			const TSharedRef<IPlugin>& Plugin = Plugins[Index];
			const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Plugin->GetName());
			Row->SetStringField(TEXT("friendlyName"), Descriptor.FriendlyName);
			Row->SetStringField(TEXT("description"), Descriptor.Description);
			Row->SetStringField(TEXT("category"), Descriptor.Category);
			Row->SetStringField(TEXT("versionName"), Descriptor.VersionName);
			Row->SetBoolField(TEXT("canContainContent"), Descriptor.bCanContainContent);
			Row->SetBoolField(TEXT("isBeta"), Descriptor.bIsBetaVersion);
			Row->SetBoolField(TEXT("isExperimental"), Descriptor.bIsExperimentalVersion);
			Row->SetBoolField(TEXT("installed"), Descriptor.bInstalled);
			Row->SetStringField(TEXT("rootDir"), MakeProjectRelative(Plugin->GetBaseDir()));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		Result->SetNumberField(TEXT("enabledPluginCount"), Plugins.Num());
		Result->SetBoolField(TEXT("truncated"), Plugins.Num() > MaxResourceRows);
		Result->SetArrayField(TEXT("plugins"), Rows);
		return SerializeObject(Result);
	}

	FString GetProjectSourceIndexResource()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);

		TArray<FString> Roots;
		const FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source")));
		const FString PluginsRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir());
		if (IFileManager::Get().DirectoryExists(*SourceRoot))
		{
			Roots.Add(SourceRoot);
		}
		if (IFileManager::Get().DirectoryExists(*PluginsRoot))
		{
			Roots.Add(PluginsRoot);
		}

		TArray<FString> Files;
		for (const FString& Root : Roots)
		{
			for (const TCHAR* Pattern : { TEXT("*.h"), TEXT("*.hpp"), TEXT("*.cpp"), TEXT("*.cs"), TEXT("*.uplugin") })
			{
				TArray<FString> Found;
				IFileManager::Get().FindFilesRecursive(Found, *Root, Pattern, true, false);
				Files.Append(Found);
			}
		}
		Files.Sort();

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 Index = 0; Index < Files.Num() && Index < MaxSourceFiles; ++Index)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), MakeProjectRelative(Files[Index]));
			Row->SetStringField(TEXT("extension"), FPaths::GetExtension(Files[Index]));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		Result->SetNumberField(TEXT("fileCount"), Files.Num());
		Result->SetBoolField(TEXT("truncated"), Files.Num() > MaxSourceFiles);
		Result->SetArrayField(TEXT("files"), Rows);
		return SerializeObject(Result);
	}

	FString GetCurrentLevelResource()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : GWorld;
		if (!World)
		{
			Result->SetStringField(TEXT("error"), TEXT("No editor world."));
			Result->SetBoolField(TEXT("success"), false);
			return SerializeObject(Result);
		}

		TMap<FString, int32> ClassCounts;
		int32 ActorCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			++ActorCount;
			const FString ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : TEXT("Unknown");
			ClassCounts.FindOrAdd(ClassName)++;
		}

		TArray<TSharedPtr<FJsonValue>> Counts;
		for (const TPair<FString, int32>& Pair : ClassCounts)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("class"), Pair.Key);
			Row->SetNumberField(TEXT("count"), Pair.Value);
			Counts.Add(MakeShared<FJsonValueObject>(Row));
		}

		Result->SetStringField(TEXT("levelName"), World->GetMapName());
		Result->SetNumberField(TEXT("actorCount"), ActorCount);
		Result->SetArrayField(TEXT("classCounts"), Counts);
		return SerializeObject(Result);
	}

	FString GetLevelComponentsResource()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : GWorld;
		if (!World)
		{
			return ErrorJson(TEXT("No editor world."));
		}

		TMap<FString, int32> ComponentCounts;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			TInlineComponentArray<UActorComponent*> Components(Actor);
			for (UActorComponent* Component : Components)
			{
				if (Component)
				{
					ComponentCounts.FindOrAdd(Component->GetClass()->GetName())++;
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TPair<FString, int32>& Pair : ComponentCounts)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("class"), Pair.Key);
			Row->SetNumberField(TEXT("count"), Pair.Value);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Result->SetArrayField(TEXT("components"), Rows);
		return SerializeObject(Result);
	}

	FString GetEditorPerformanceResource()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("memoryUsedMB"), FPlatformMemory::GetStats().UsedPhysical / 1024.0 / 1024.0);
		Result->SetNumberField(TEXT("memoryPeakMB"), FPlatformMemory::GetStats().PeakUsedPhysical / 1024.0 / 1024.0);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : GWorld;
		int32 ActorCount = 0;
		if (World)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				++ActorCount;
			}
		}
		Result->SetNumberField(TEXT("actorCount"), ActorCount);
		return SerializeObject(Result);
	}

	FString GetViewportResource()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("message"), TEXT("Viewport camera details are not exposed by this standalone UEBridgeMCP resource yet."));
		return SerializeObject(Result);
	}

	void FindRecipeRoots(TArray<FString>& OutRoots)
	{
		const FString UEBridgeRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("pcg_tool"), TEXT("recipe_library")));
		OutRoots.Add(UEBridgeRoot);
	}

	FString GetActiveRecipeRoot()
	{
		TArray<FString> Roots;
		FindRecipeRoots(Roots);
		for (const FString& Root : Roots)
		{
			if (IFileManager::Get().DirectoryExists(*Root))
			{
				return Root;
			}
		}
		return Roots.Num() > 0 ? Roots[0] : FString();
	}

	FString NormalizeFullFilePath(FString Path)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::CollapseRelativeDirectories(Path);
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	FString NormalizeFullDirectoryPath(FString Path)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::CollapseRelativeDirectories(Path);
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	bool IsPathInsideDirectory(const FString& Path, const FString& Directory)
	{
		const FString FullPath = NormalizeFullFilePath(Path);
		const FString FullDirectory = NormalizeFullDirectoryPath(Directory);
		return FullPath.Equals(FullDirectory, ESearchCase::IgnoreCase)
			|| FullPath.StartsWith(FullDirectory + TEXT("/"), ESearchCase::IgnoreCase);
	}

	bool ValidateRecipeJsonFilePath(const FString& FullPath, const FString& RecipeRoot, FString& OutError)
	{
		if (!IsPathInsideDirectory(FullPath, RecipeRoot))
		{
			OutError = FString::Printf(TEXT("Recipe file must stay inside the active recipe root: %s"), *MakeProjectRelative(FullPath));
			return false;
		}

		if (!FPaths::GetExtension(FullPath, true).Equals(TEXT(".json"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Recipe file must be a .json file: %s"), *MakeProjectRelative(FullPath));
			return false;
		}

		const int64 FileSize = IFileManager::Get().FileSize(*FullPath);
		if (FileSize >= 0 && FileSize > MaxRecipeJsonReadBytes)
		{
			OutError = FString::Printf(TEXT("Recipe file is larger than the %s byte read limit: %s"), *LexToString(MaxRecipeJsonReadBytes), *MakeProjectRelative(FullPath));
			return false;
		}
		return true;
	}

	bool ResolveRecipeJsonFilePath(const FString& FileArg, const FString& RecipeRoot, FString& OutFullPath, FString& OutError)
	{
		if (RecipeRoot.IsEmpty())
		{
			OutError = TEXT("Active recipe root is not configured.");
			return false;
		}
		if (FileArg.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("Recipe file path is required.");
			return false;
		}

		FString Candidate;
		FString ProjectPath;
		FString ProjectPathError;
		if (ResolveProjectFilePath(FileArg, ProjectPath, ProjectPathError))
		{
			Candidate = ProjectPath;
		}
		else
		{
			Candidate = FPaths::IsRelative(FileArg)
				? FPaths::Combine(RecipeRoot, FileArg)
				: FileArg;
			Candidate = NormalizeFullFilePath(Candidate);
		}

		if (!ValidateRecipeJsonFilePath(Candidate, RecipeRoot, OutError))
		{
			return false;
		}

		OutFullPath = Candidate;
		return true;
	}

	void FindJsonFilesRecursive(const FString& Root, TArray<FString>& OutFiles)
	{
		if (IFileManager::Get().DirectoryExists(*Root))
		{
			IFileManager::Get().FindFilesRecursive(OutFiles, *Root, TEXT("*.json"), true, false);
			OutFiles.Sort();
		}
	}

	void GetStringArrayField(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Args.IsValid() || !Args->TryGetArrayField(FieldName, Values) || !Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text = Value.IsValid() ? Value->AsString() : FString();
			if (!Text.IsEmpty())
			{
				OutValues.Add(Text);
			}
		}
	}

	bool JsonObjectMatchesId(const TSharedPtr<FJsonObject>& Object, const FString& WantedId)
	{
		if (!Object.IsValid() || WantedId.IsEmpty())
		{
			return false;
		}

		for (const TCHAR* FieldName : { TEXT("id"), TEXT("recipe_id"), TEXT("binding_id"), TEXT("name") })
		{
			FString Value;
			if (Object->TryGetStringField(FieldName, Value) && Value.Equals(WantedId, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	FString FindJsonFileByIdOrName(const FString& Wanted, const TArray<FString>& Files)
	{
		if (Wanted.IsEmpty())
		{
			return FString();
		}

		for (const FString& File : Files)
		{
			const FString Base = FPaths::GetBaseFilename(File);
			if (Base.Equals(Wanted, ESearchCase::IgnoreCase) || File.Contains(Wanted, ESearchCase::IgnoreCase))
			{
				return File;
			}

			TSharedPtr<FJsonObject> Object = LoadJsonObjectFile(File);
			if (JsonObjectMatchesId(Object, Wanted))
			{
				return File;
			}
		}
		return FString();
	}

	FString ReadJsonFileAsResult(const FString& File, const FString& FieldName)
	{
		const int64 FileSize = IFileManager::Get().FileSize(*File);
		if (FileSize > MaxRecipeJsonReadBytes)
		{
			return ErrorJson(FString::Printf(TEXT("Recipe file is larger than the %s byte read limit: %s"), *LexToString(MaxRecipeJsonReadBytes), *MakeProjectRelative(File)));
		}

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *File))
		{
			return ErrorJson(FString::Printf(TEXT("Failed to read file: %s"), *File));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("file"), MakeProjectRelative(File));

		TSharedPtr<FJsonObject> Object = ParseJsonObject(Content);
		if (Object.IsValid())
		{
			Result->SetObjectField(FieldName, Object);
		}
		else
		{
			Result->SetStringField(TEXT("content"), Content);
		}
		return SerializeObject(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"list_resources","description":"List MCP resources using worlddata:// URIs (same catalog as the MCP resources/list method).","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"List Resources","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_resource","description":"Read an MCP resource by worlddata:// URI. Recommended first read: worlddata://context/bootstrap. Legacy ubridge:// URIs are still accepted.","inputSchema":{"type":"object","properties":{"uri":{"type":"string"}},"required":["uri"]},"annotations":{"title":"Read Resource","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_current_task_context","description":"Return a compact task-oriented context snapshot: project identity, current level, selected actors, dirty packages, recent log lines, and recommended next reads.","inputSchema":{"type":"object","properties":{"maxSelectedActors":{"type":"number"},"maxDirtyPackages":{"type":"number"}}},"annotations":{"title":"Current Task Context","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_relevant_context","description":"Search current editor context by a task query and return matching actors, assets, source files, and log lines. Use this before broad scans.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"maxResults":{"type":"number"}},"required":["query"]},"annotations":{"title":"Relevant Context","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_asset_references","description":"Return AssetRegistry dependencies and referencers for an asset path/package. Useful for expanding asset relationship context.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"path":{"type":"string"},"includeDependencies":{"type":"boolean"},"includeReferencers":{"type":"boolean"},"maxResults":{"type":"number"}}},"annotations":{"title":"Asset References","readOnlyHint":true,"openWorldHint":false}},
{"name":"summarize_blueprint","description":"Summarize a Blueprint asset: parent/generated class, graph names and node counts, variables, and compact references.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"path":{"type":"string"},"maxGraphs":{"type":"number"},"maxVariables":{"type":"number"}}},"annotations":{"title":"Blueprint Summary","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_log","description":"Read recent Unreal log lines from the project Saved/Logs folder. Supports lines, severity, and category filters.","inputSchema":{"type":"object","properties":{"lines":{"type":"number","description":"Number of lines. Default 50."},"severity":{"type":"string","description":"Filter: Error, Warning, Log."},"category":{"type":"string"}}},"annotations":{"title":"Read Log","readOnlyHint":true,"openWorldHint":false}},
{"name":"execute_python","description":"Execute Python code in the Unreal Editor through PythonScriptPlugin.","inputSchema":{"type":"object","properties":{"code":{"type":"string","description":"Python code to execute."}},"required":["code"]},"annotations":{"title":"Execute Python","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"find_static_meshes","description":"Search StaticMesh assets for placement or PCG use.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"searchTerm":{"type":"string"},"path":{"type":"string"},"maxResults":{"type":"number"}}},"annotations":{"title":"Find Static Meshes","readOnlyHint":true,"openWorldHint":false}},
{"name":"list_project_modules","description":"List project module source folders and Build.cs files.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"List Project Modules","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_build_configuration","description":"Get build/platform/editor configuration for the current session.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Build Configuration","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_file","description":"Read a text file inside the project directory.","inputSchema":{"type":"object","properties":{"file_path":{"type":"string"}},"required":["file_path"]},"annotations":{"title":"Read File","readOnlyHint":true,"openWorldHint":false}},
{"name":"write_file","description":"Write a text file inside the project directory.","inputSchema":{"type":"object","properties":{"file_path":{"type":"string"},"content":{"type":"string"}},"required":["file_path","content"]},"annotations":{"title":"Write File","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"delete_file","description":"Delete a file inside the project directory.","inputSchema":{"type":"object","properties":{"file_path":{"type":"string"}},"required":["file_path"]},"annotations":{"title":"Delete File","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"rename_file","description":"Rename or move a file inside the project directory.","inputSchema":{"type":"object","properties":{"old_path":{"type":"string"},"new_path":{"type":"string"}},"required":["old_path","new_path"]},"annotations":{"title":"Rename File","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"play_in_editor","description":"Start Play in Editor.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Play In Editor","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"stop_pie","description":"Stop Play in Editor.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Stop PIE","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"pcg_recipe_library_status","description":"Read the standalone UEBridgeMCP PCG recipe library status from Saved/UEBridgeMCP/pcg_tool/recipe_library.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"PCG Recipe Library Status","readOnlyHint":true,"openWorldHint":false}},
{"name":"search_pcg_recipes","description":"Search normalized PCG recipe JSON files by query, tags, scene inputs, or output layers.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"tags":{"type":"array","items":{"type":"string"}},"required_scene_inputs":{"type":"array","items":{"type":"string"}},"output_layers":{"type":"array","items":{"type":"string"}},"limit":{"type":"number"},"include_recipe":{"type":"boolean"}}},"annotations":{"title":"Search PCG Recipes","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_pcg_recipe","description":"Read a normalized PCG recipe by id, recipe_id, or file.","inputSchema":{"type":"object","properties":{"id":{"type":"string"},"recipe_id":{"type":"string"},"file":{"type":"string"},"include_scene_binding":{"type":"boolean"}}},"annotations":{"title":"Read PCG Recipe","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_pcg_scene_binding","description":"Read a PCG scene-binding contract by recipe_id, binding_id, or file.","inputSchema":{"type":"object","properties":{"recipe_id":{"type":"string"},"binding_id":{"type":"string"},"file":{"type":"string"}}},"annotations":{"title":"Read PCG Scene Binding","readOnlyHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("list_resources")) { OutResult = ListResources(); return true; }
	if (ToolName == TEXT("read_resource")) { OutResult = ReadResource(Args); return true; }
	if (ToolName == TEXT("get_current_task_context")) { OutResult = GetCurrentTaskContext(Args); return true; }
	if (ToolName == TEXT("get_relevant_context")) { OutResult = GetRelevantContext(Args); return true; }
	if (ToolName == TEXT("get_asset_references")) { OutResult = GetAssetReferences(Args); return true; }
	if (ToolName == TEXT("summarize_blueprint")) { OutResult = SummarizeBlueprint(Args); return true; }
	if (ToolName == TEXT("read_log")) { OutResult = ReadLog(Args); return true; }
	if (ToolName == TEXT("execute_python")) { OutResult = ExecutePython(Args); return true; }
	// search_assets / get_level_actors / get_project_info are intentionally no longer advertised
	// in GetToolDefinitionsJson (they duplicate find_assets / list_level_actors /
	// get_current_project_info). They remain dispatchable here for backward compatibility.
	if (ToolName == TEXT("search_assets")) { OutResult = SearchAssets(Args); return true; }
	if (ToolName == TEXT("find_static_meshes")) { OutResult = FindStaticMeshes(Args); return true; }
	if (ToolName == TEXT("get_level_actors")) { OutResult = GetLevelActors(Args); return true; }
	if (ToolName == TEXT("get_project_info")) { OutResult = GetProjectInfo(Args); return true; }
	if (ToolName == TEXT("list_project_modules")) { OutResult = ListProjectModules(Args); return true; }
	if (ToolName == TEXT("get_build_configuration")) { OutResult = GetBuildConfiguration(Args); return true; }
	if (ToolName == TEXT("read_file")) { OutResult = ReadFile(Args); return true; }
	if (ToolName == TEXT("write_file")) { OutResult = WriteFile(Args); return true; }
	if (ToolName == TEXT("delete_file")) { OutResult = DeleteFile(Args); return true; }
	if (ToolName == TEXT("rename_file")) { OutResult = RenameFile(Args); return true; }
	if (ToolName == TEXT("play_in_editor")) { OutResult = PlayInEditor(Args); return true; }
	if (ToolName == TEXT("stop_pie")) { OutResult = StopPIE(Args); return true; }
	if (ToolName == TEXT("pcg_recipe_library_status")) { OutResult = PcgRecipeLibraryStatus(Args); return true; }
	if (ToolName == TEXT("search_pcg_recipes")) { OutResult = SearchPcgRecipes(Args); return true; }
	if (ToolName == TEXT("read_pcg_recipe")) { OutResult = ReadPcgRecipe(Args); return true; }
	if (ToolName == TEXT("read_pcg_scene_binding")) { OutResult = ReadPcgSceneBinding(Args); return true; }
	return false;
}

FString ListResources()
{
	// Canonical worlddata:// resource catalog (17 entries). Both the MCP resources/list
	// method and the list_resources tool surface this exact set — one source of truth.
	TArray<TSharedPtr<FJsonValue>> Resources;
	AddResource(Resources, TEXT("worlddata://context/bootstrap"), TEXT("Bootstrap Context"), TEXT("Recommended first-read order and compact editor state."));
	AddResource(Resources, TEXT("worlddata://context/current-task"), TEXT("Current Task Context"), TEXT("Task-oriented project, level, selection, dirty-package, and recent-log snapshot."));
	AddResource(Resources, TEXT("worlddata://project/info"), TEXT("Project Info"), TEXT("Engine version, project name, paths, and MCP endpoint."));
	AddResource(Resources, TEXT("worlddata://codex/policy-snapshot"), TEXT("Codex Policy Snapshot"), TEXT("Redacted local Codex config policy and MCP server configuration."));
	AddResource(Resources, TEXT("worlddata://project/plugins"), TEXT("Project Plugins"), TEXT("Enabled plugin inventory and plugin metadata."));
	AddResource(Resources, TEXT("worlddata://project/source-index"), TEXT("Project Source Index"), TEXT("Source and plugin code file index."));
	AddResource(Resources, TEXT("worlddata://content/assets"), TEXT("Content Assets"), TEXT("Asset registry survey under /Game."));
	AddResource(Resources, TEXT("worlddata://content/summary"), TEXT("Content Summary"), TEXT("Asset counts by class under /Game."));
	AddResource(Resources, TEXT("worlddata://level/current"), TEXT("Current Level"), TEXT("Current map summary and actor class distribution."));
	AddResource(Resources, TEXT("worlddata://level/actors"), TEXT("Level Actors"), TEXT("Current editor-world actors and transforms."));
	AddResource(Resources, TEXT("worlddata://level/components"), TEXT("Level Components"), TEXT("Component class distribution in the current world."));
	AddResource(Resources, TEXT("worlddata://blueprints/index"), TEXT("Blueprint Index"), TEXT("Blueprint asset inventory."));
	AddResource(Resources, TEXT("worlddata://pcg/graphs"), TEXT("PCG Graphs"), TEXT("PCG graph asset inventory."));
	AddResource(Resources, TEXT("worlddata://editor/problems"), TEXT("Editor Problems"), TEXT("Recent warning/error log lines."));
	AddResource(Resources, TEXT("worlddata://editor/selection"), TEXT("Editor Selection"), TEXT("Currently selected actors."));
	AddResource(Resources, TEXT("worlddata://editor/performance"), TEXT("Performance Stats"), TEXT("Memory and actor-count snapshot."));
	AddResource(Resources, TEXT("worlddata://editor/log"), TEXT("Editor Log"), TEXT("Recent project log lines."));
	AddResource(Resources, TEXT("worlddata://editor/viewport"), TEXT("Viewport Info"), TEXT("Viewport availability summary."));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("source"), TEXT("UEBridgeMCP"));
	Result->SetStringField(TEXT("recommendedFirstRead"), TEXT("worlddata://context/bootstrap"));
	Result->SetArrayField(TEXT("resources"), Resources);
	return SerializeObject(Result);
}

FString ReadResource(const TSharedPtr<FJsonObject>& Args)
{
	FString Uri = GetStringField(Args, TEXT("uri"));
	if (Uri.IsEmpty())
	{
		return ErrorJson(TEXT("uri is required."));
	}
	// Back-compat: the legacy ubridge:// scheme aliases the canonical worlddata:// catalog.
	if (Uri.StartsWith(TEXT("ubridge://")))
	{
		Uri = TEXT("worlddata://") + Uri.RightChop(10);
	}
	// Single source of truth: the server owns read dispatch and routes the resources
	// implemented here back via ReadExtendedResource().
	return FWorldDataMCPServer::ReadResource(Uri);
}

FString ReadExtendedResource(const FString& Uri)
{
	if (Uri == TEXT("worlddata://context/current-task")) return GetCurrentTaskContext(MakeShared<FJsonObject>());
	if (Uri == TEXT("worlddata://project/plugins")) return GetProjectPluginsResource();
	if (Uri == TEXT("worlddata://project/source-index")) return GetProjectSourceIndexResource();
	if (Uri == TEXT("worlddata://level/current")) return GetCurrentLevelResource();
	if (Uri == TEXT("worlddata://level/components")) return GetLevelComponentsResource();
	if (Uri == TEXT("worlddata://blueprints/index"))
	{
		TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
		Query->SetStringField(TEXT("classFilter"), TEXT("Blueprint"));
		Query->SetNumberField(TEXT("maxResults"), MaxResourceRows);
		return WorldDataMCP::Tools::FindAssets(Query);
	}
	if (Uri == TEXT("worlddata://pcg/graphs"))
	{
		TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
		Query->SetStringField(TEXT("classFilter"), TEXT("PCG"));
		Query->SetNumberField(TEXT("maxResults"), MaxResourceRows);
		return WorldDataMCP::Tools::FindAssets(Query);
	}
	if (Uri == TEXT("worlddata://editor/problems"))
	{
		TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
		Query->SetNumberField(TEXT("lines"), 80);
		return ReadLog(Query);
	}
	if (Uri == TEXT("worlddata://editor/performance")) return GetEditorPerformanceResource();
	if (Uri == TEXT("worlddata://editor/log")) return ReadLog(MakeShared<FJsonObject>());
	if (Uri == TEXT("worlddata://editor/viewport")) return GetViewportResource();

	return ErrorJson(FString::Printf(TEXT("Unknown resource: %s"), *Uri));
}

FString ReadLog(const TSharedPtr<FJsonObject>& Args)
{
	int32 LineCount = FMath::Clamp(static_cast<int32>(GetNumberField(Args, TEXT("lines"), 50)), 1, 1000);
	const FString Severity = GetStringField(Args, TEXT("severity"));
	const FString Category = GetStringField(Args, TEXT("category"));

	const FString LogDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
	TArray<FString> LogFiles;
	IFileManager::Get().FindFilesRecursive(LogFiles, *LogDir, TEXT("*.log"), true, false);
	if (LogFiles.Num() == 0)
	{
		return ErrorJson(FString::Printf(TEXT("No log files found in %s"), *LogDir));
	}

	Algo::Sort(LogFiles, [](const FString& A, const FString& B)
	{
		return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
	});

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *LogFiles[0]))
	{
		return ErrorJson(FString::Printf(TEXT("Failed to read log file: %s"), *LogFiles[0]));
	}

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines, false);

	TArray<TSharedPtr<FJsonValue>> OutLines;
	for (int32 Index = Lines.Num() - 1; Index >= 0 && OutLines.Num() < LineCount; --Index)
	{
		const FString& Line = Lines[Index];
		if (!Category.IsEmpty() && !Line.Contains(Category, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!Severity.IsEmpty())
		{
			const bool bMatchesSeverity =
				Line.Contains(FString::Printf(TEXT("[%s]"), *Severity), ESearchCase::IgnoreCase)
				|| Line.Contains(FString::Printf(TEXT("%s:"), *Severity), ESearchCase::IgnoreCase)
				|| Line.Contains(Severity, ESearchCase::IgnoreCase);
			if (!bMatchesSeverity)
			{
				continue;
			}
		}
		OutLines.Insert(MakeShared<FJsonValueString>(Line), 0);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("logFile"), MakeProjectRelative(LogFiles[0]));
	Result->SetNumberField(TEXT("count"), OutLines.Num());
	Result->SetArrayField(TEXT("lines"), OutLines);
	return SerializeObject(Result);
}

FString GetCurrentTaskContext(const TSharedPtr<FJsonObject>& Args)
{
	const int32 MaxSelectedActors = FMath::Clamp(static_cast<int32>(GetNumberField(Args, TEXT("maxSelectedActors"), 20)), 1, 100);
	const int32 MaxDirtyPackages = FMath::Clamp(static_cast<int32>(GetNumberField(Args, TEXT("maxDirtyPackages"), 80)), 1, 300);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("purpose"), TEXT("Compact task context: current project, editor world, selection, dirty packages, recent problems, and next reads."));
	Result->SetStringField(TEXT("generatedAtUtc"), FDateTime::UtcNow().ToIso8601());

	TSharedRef<FJsonObject> Project = MakeShared<FJsonObject>();
	Project->SetStringField(TEXT("projectId"), FWorldDataMCPServer::GetProjectId());
	Project->SetStringField(TEXT("serverName"), FWorldDataMCPServer::GetServerName());
	Project->SetStringField(TEXT("mcpUrl"), FWorldDataMCPServer::GetMcpUrl());
	Project->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Result->SetObjectField(TEXT("project"), Project);

	UWorld* World = GetEditorWorld();
	TSharedRef<FJsonObject> Editor = MakeShared<FJsonObject>();
	if (World)
	{
		int32 ActorCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++ActorCount;
			}
		}

		Editor->SetStringField(TEXT("levelName"), World->GetMapName());
		Editor->SetStringField(TEXT("levelPackage"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
		Editor->SetNumberField(TEXT("actorCount"), ActorCount);
	}
	Editor->SetBoolField(TEXT("isPlayInEditor"), GEditor ? (GEditor->PlayWorld != nullptr) : false);
	Editor->SetNumberField(TEXT("selectedActorCount"), GEditor ? GEditor->GetSelectedActorCount() : 0);
	Result->SetObjectField(TEXT("editor"), Editor);

	TArray<TSharedPtr<FJsonValue>> SelectedActors;
	if (GEditor)
	{
		if (USelection* Selection = GEditor->GetSelectedActors())
		{
			TArray<AActor*> Selected;
			Selection->GetSelectedObjects<AActor>(Selected);
			for (AActor* Actor : Selected)
			{
				if (IsValid(Actor) && SelectedActors.Num() < MaxSelectedActors)
				{
					SelectedActors.Add(MakeShared<FJsonValueObject>(MakeCompactActorObject(Actor)));
				}
			}
			Result->SetBoolField(TEXT("selectedActorsTruncated"), Selected.Num() > SelectedActors.Num());
		}
	}
	Result->SetArrayField(TEXT("selectedActors"), SelectedActors);

	TArray<TSharedPtr<FJsonValue>> DirtyPackages;
	int32 DirtyPackageCount = 0;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (!Package || !Package->IsDirty())
		{
			continue;
		}

		const FString PackageName = Package->GetName();
		if (!PackageName.StartsWith(TEXT("/Game")) && !PackageName.StartsWith(TEXT("/Script")) && !PackageName.StartsWith(TEXT("/Engine")))
		{
			continue;
		}

		++DirtyPackageCount;
		if (DirtyPackages.Num() < MaxDirtyPackages)
		{
			DirtyPackages.Add(MakeShared<FJsonValueString>(PackageName));
		}
	}
	Result->SetNumberField(TEXT("dirtyPackageCount"), DirtyPackageCount);
	Result->SetBoolField(TEXT("dirtyPackagesTruncated"), DirtyPackageCount > DirtyPackages.Num());
	Result->SetArrayField(TEXT("dirtyPackages"), DirtyPackages);

	TSharedPtr<FJsonObject> ProblemsArgs = MakeShared<FJsonObject>();
	ProblemsArgs->SetNumberField(TEXT("lines"), 80);
	TSharedPtr<FJsonObject> Problems = ParseToolJson(ReadLog(ProblemsArgs));
	if (Problems.IsValid())
	{
		Result->SetObjectField(TEXT("recentLog"), Problems);
	}

	TArray<TSharedPtr<FJsonValue>> RecommendedReads;
	RecommendedReads.Add(MakeShared<FJsonValueString>(TEXT("worlddata://editor/selection")));
	RecommendedReads.Add(MakeShared<FJsonValueString>(TEXT("worlddata://level/current")));
	RecommendedReads.Add(MakeShared<FJsonValueString>(TEXT("worlddata://editor/problems")));
	RecommendedReads.Add(MakeShared<FJsonValueString>(TEXT("worlddata://project/source-index")));
	RecommendedReads.Add(MakeShared<FJsonValueString>(TEXT("Use get_relevant_context with the user's task terms before broad scans.")));
	Result->SetArrayField(TEXT("recommendedNextReads"), RecommendedReads);

	return SerializeObject(Result);
}

FString GetRelevantContext(const TSharedPtr<FJsonObject>& Args)
{
	const FString Query = GetStringField(Args, TEXT("query"));
	const int32 MaxResults = FMath::Clamp(static_cast<int32>(GetNumberField(Args, TEXT("maxResults"), 20)), 1, 100);
	if (Query.TrimStartAndEnd().IsEmpty())
	{
		return GetCurrentTaskContext(Args);
	}

	const FString LowerQuery = Query.ToLower();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("query"), Query);
	Result->SetStringField(TEXT("generatedAtUtc"), FDateTime::UtcNow().ToIso8601());

	TArray<TSharedPtr<FJsonValue>> Actors;
	int32 ActorMatchedCount = 0;
	CollectMatchingActors(Query, MaxResults, Actors, ActorMatchedCount);
	Result->SetNumberField(TEXT("actorMatchedCount"), ActorMatchedCount);
	Result->SetBoolField(TEXT("actorsTruncated"), ActorMatchedCount > Actors.Num());
	Result->SetArrayField(TEXT("actors"), Actors);

	TArray<TSharedPtr<FJsonValue>> Assets;
	int32 AssetMatchedCount = 0;
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(TEXT("/Game")));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssets(Filter, AssetDataList);
	for (const FAssetData& AssetData : AssetDataList)
	{
		const FString Probe = FString::Printf(TEXT("%s %s %s %s"),
			*AssetData.AssetName.ToString(),
			*AssetData.GetObjectPathString(),
			*AssetData.PackageName.ToString(),
			*AssetData.AssetClassPath.GetAssetName().ToString()).ToLower();
		if (!Probe.Contains(LowerQuery))
		{
			continue;
		}

		++AssetMatchedCount;
		if (Assets.Num() < MaxResults)
		{
			Assets.Add(MakeShared<FJsonValueObject>(MakeAssetObject(AssetData)));
		}
	}
	Result->SetNumberField(TEXT("assetMatchedCount"), AssetMatchedCount);
	Result->SetBoolField(TEXT("assetsTruncated"), AssetMatchedCount > Assets.Num());
	Result->SetArrayField(TEXT("assets"), Assets);

	TArray<TSharedPtr<FJsonValue>> SourceFiles;
	int32 SourceMatchedCount = 0;
	CollectMatchingSourceFiles(Query, MaxResults, SourceFiles, SourceMatchedCount);
	Result->SetNumberField(TEXT("sourceMatchedCount"), SourceMatchedCount);
	Result->SetBoolField(TEXT("sourceFilesTruncated"), SourceMatchedCount > SourceFiles.Num());
	Result->SetArrayField(TEXT("sourceFiles"), SourceFiles);

	TSharedPtr<FJsonObject> LogArgs = MakeShared<FJsonObject>();
	LogArgs->SetNumberField(TEXT("lines"), 200);
	TSharedPtr<FJsonObject> Log = ParseToolJson(ReadLog(LogArgs));
	TArray<TSharedPtr<FJsonValue>> MatchingLogLines;
	int32 LogMatchedCount = 0;
	if (Log.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Lines = nullptr;
		if (Log->TryGetArrayField(TEXT("lines"), Lines) && Lines)
		{
			for (const TSharedPtr<FJsonValue>& LineValue : *Lines)
			{
				const FString Line = LineValue.IsValid() ? LineValue->AsString() : FString();
				if (!Line.ToLower().Contains(LowerQuery))
				{
					continue;
				}
				++LogMatchedCount;
				if (MatchingLogLines.Num() < MaxResults)
				{
					MatchingLogLines.Add(MakeShared<FJsonValueString>(Line));
				}
			}
		}
	}
	Result->SetNumberField(TEXT("logMatchedCount"), LogMatchedCount);
	Result->SetBoolField(TEXT("logLinesTruncated"), LogMatchedCount > MatchingLogLines.Num());
	Result->SetArrayField(TEXT("logLines"), MatchingLogLines);

	TArray<TSharedPtr<FJsonValue>> FollowUps;
	FollowUps.Add(MakeShared<FJsonValueString>(TEXT("For a matching asset, call get_asset_references to expand dependencies/referencers.")));
	FollowUps.Add(MakeShared<FJsonValueString>(TEXT("For a matching Blueprint asset, call summarize_blueprint for graph and variable structure.")));
	Result->SetArrayField(TEXT("followUpTools"), FollowUps);

	return SerializeObject(Result);
}

FString GetAssetReferences(const TSharedPtr<FJsonObject>& Args)
{
	const FString AssetPath = !GetStringField(Args, TEXT("assetPath")).IsEmpty()
		? GetStringField(Args, TEXT("assetPath"))
		: GetStringField(Args, TEXT("path"));
	const int32 MaxResults = FMath::Clamp(static_cast<int32>(GetNumberField(Args, TEXT("maxResults"), 100)), 1, 500);
	const bool bIncludeDependencies = GetBoolField(Args, TEXT("includeDependencies"), true);
	const bool bIncludeReferencers = GetBoolField(Args, TEXT("includeReferencers"), true);

	FAssetData AssetData;
	FString Error;
	if (!ResolveAssetDataByPath(AssetPath, AssetData, Error))
	{
		return ErrorJson(Error);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("asset"), MakeAssetObject(AssetData));

	auto AddReferenceArray = [&AssetRegistry, MaxResults, &Result](const TCHAR* FieldName, const TCHAR* CountFieldName, const TCHAR* TruncatedFieldName, TArray<FName>& PackageNames)
	{
		PackageNames.Sort([](const FName& A, const FName& B)
		{
			return A.ToString() < B.ToString();
		});

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 Index = 0; Index < PackageNames.Num() && Rows.Num() < MaxResults; ++Index)
		{
			Rows.Add(MakeShared<FJsonValueObject>(MakePackageReferenceObject(AssetRegistry, PackageNames[Index])));
		}
		Result->SetNumberField(CountFieldName, PackageNames.Num());
		Result->SetBoolField(TruncatedFieldName, PackageNames.Num() > Rows.Num());
		Result->SetArrayField(FieldName, Rows);
	};

	if (bIncludeDependencies)
	{
		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(AssetData.PackageName, Dependencies);
		AddReferenceArray(TEXT("dependencies"), TEXT("dependencyCount"), TEXT("dependenciesTruncated"), Dependencies);
	}
	if (bIncludeReferencers)
	{
		TArray<FName> Referencers;
		AssetRegistry.GetReferencers(AssetData.PackageName, Referencers);
		AddReferenceArray(TEXT("referencers"), TEXT("referencerCount"), TEXT("referencersTruncated"), Referencers);
	}

	return SerializeObject(Result);
}

FString SummarizeBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	const FString AssetPath = !GetStringField(Args, TEXT("assetPath")).IsEmpty()
		? GetStringField(Args, TEXT("assetPath"))
		: GetStringField(Args, TEXT("path"));
	const int32 MaxGraphs = FMath::Clamp(static_cast<int32>(GetNumberField(Args, TEXT("maxGraphs"), 40)), 1, 200);
	const int32 MaxVariables = FMath::Clamp(static_cast<int32>(GetNumberField(Args, TEXT("maxVariables"), 80)), 1, 300);

	FAssetData AssetData;
	FString Error;
	if (!ResolveAssetDataByPath(AssetPath, AssetData, Error))
	{
		return ErrorJson(Error);
	}

	UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
	if (!Blueprint)
	{
		Blueprint = LoadObject<UBlueprint>(nullptr, *NormalizeAssetObjectPath(AssetData.GetObjectPathString()));
	}
	if (!Blueprint)
	{
		return ErrorJson(FString::Printf(TEXT("Asset is not a loaded Blueprint: %s"), *AssetPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("asset"), MakeAssetObject(AssetData));
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : FString());
	Result->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetName() : FString());
	Result->SetStringField(TEXT("skeletonGeneratedClass"), Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass->GetName() : FString());

#if WITH_EDITORONLY_DATA
	auto AddGraphs = [MaxGraphs](const TArray<TObjectPtr<UEdGraph>>& Graphs, const FString& Kind, TArray<TSharedPtr<FJsonValue>>& OutRows, int32& OutTotal)
	{
		OutTotal += Graphs.Num();
		for (const TObjectPtr<UEdGraph>& GraphPtr : Graphs)
		{
			const UEdGraph* Graph = GraphPtr.Get();
			if (!Graph || OutRows.Num() >= MaxGraphs)
			{
				continue;
			}

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Graph->GetName());
			Row->SetStringField(TEXT("kind"), Kind);
			Row->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
			OutRows.Add(MakeShared<FJsonValueObject>(Row));
		}
	};

	TArray<TSharedPtr<FJsonValue>> GraphRows;
	int32 TotalGraphCount = 0;
	AddGraphs(Blueprint->UbergraphPages, TEXT("event"), GraphRows, TotalGraphCount);
	AddGraphs(Blueprint->FunctionGraphs, TEXT("function"), GraphRows, TotalGraphCount);
	AddGraphs(Blueprint->MacroGraphs, TEXT("macro"), GraphRows, TotalGraphCount);
	AddGraphs(Blueprint->DelegateSignatureGraphs, TEXT("delegate"), GraphRows, TotalGraphCount);
	Result->SetNumberField(TEXT("graphCount"), TotalGraphCount);
	Result->SetBoolField(TEXT("graphsTruncated"), TotalGraphCount > GraphRows.Num());
	Result->SetArrayField(TEXT("graphs"), GraphRows);

	TArray<TSharedPtr<FJsonValue>> VariableRows;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		if (VariableRows.Num() >= MaxVariables)
		{
			break;
		}

		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Variable.VarName.ToString());
		Row->SetStringField(TEXT("category"), Variable.Category.ToString());
		Row->SetStringField(TEXT("pinCategory"), Variable.VarType.PinCategory.ToString());
		if (UObject* SubCategoryObject = Variable.VarType.PinSubCategoryObject.Get())
		{
			Row->SetStringField(TEXT("pinSubCategoryObject"), SubCategoryObject->GetName());
		}
		VariableRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetNumberField(TEXT("variableCount"), Blueprint->NewVariables.Num());
	Result->SetBoolField(TEXT("variablesTruncated"), Blueprint->NewVariables.Num() > VariableRows.Num());
	Result->SetArrayField(TEXT("variables"), VariableRows);
#else
	Result->SetStringField(TEXT("editorOnlyData"), TEXT("Blueprint graph and variable details are unavailable without editor-only data."));
#endif

	TSharedPtr<FJsonObject> RefArgs = MakeShared<FJsonObject>();
	RefArgs->SetStringField(TEXT("assetPath"), AssetData.GetObjectPathString());
	RefArgs->SetNumberField(TEXT("maxResults"), 25);
	TSharedPtr<FJsonObject> References = ParseToolJson(GetAssetReferences(RefArgs));
	if (References.IsValid())
	{
		Result->SetObjectField(TEXT("references"), References);
	}

	return SerializeObject(Result);
}

FString ExecutePython(const TSharedPtr<FJsonObject>& Args)
{
	const FString Code = GetStringField(Args, TEXT("code"));
	if (Code.IsEmpty())
	{
		return ErrorJson(TEXT("code is required."));
	}

	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python)
	{
		return ErrorJson(TEXT("PythonScriptPlugin is not available."));
	}

	FPythonCommandEx Command;
	Command.Command = Code;
	Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	Command.FileExecutionScope = EPythonFileExecutionScope::Private;
	Command.Flags = EPythonCommandFlags::Unattended;

	const double StartSeconds = FPlatformTime::Seconds();
	const bool bSuccess = Python->ExecPythonCommandEx(Command);
	const double DurationMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	FString CommandResult = Command.CommandResult;
	bool bCommandResultTruncated = false;
	if (CommandResult.Len() > MaxPythonResultChars)
	{
		CommandResult.LeftInline(MaxPythonResultChars);
		bCommandResultTruncated = true;
	}

	TArray<TSharedPtr<FJsonValue>> LogOutput;
	const int32 LogCount = FMath::Min(Command.LogOutput.Num(), MaxPythonLogEntries);
	for (int32 Index = 0; Index < LogCount; ++Index)
	{
		const FPythonLogOutputEntry& Entry = Command.LogOutput[Index];
		FString Output = Entry.Output;
		bool bOutputTruncated = false;
		if (Output.Len() > MaxPythonOutputChars)
		{
			Output.LeftInline(MaxPythonOutputChars);
			bOutputTruncated = true;
		}

		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("type"), LexToString(Entry.Type));
		Row->SetStringField(TEXT("output"), Output);
		Row->SetBoolField(TEXT("truncated"), bOutputTruncated);
		LogOutput.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(TEXT("message"), bSuccess ? TEXT("Python command executed.") : TEXT("Python command failed."));
	Result->SetNumberField(TEXT("durationMs"), DurationMs);
	Result->SetStringField(TEXT("commandResult"), CommandResult);
	Result->SetBoolField(TEXT("commandResultTruncated"), bCommandResultTruncated);
	Result->SetNumberField(TEXT("logCount"), Command.LogOutput.Num());
	Result->SetBoolField(TEXT("logTruncated"), Command.LogOutput.Num() > MaxPythonLogEntries);
	Result->SetArrayField(TEXT("logOutput"), LogOutput);
	return SerializeObject(Result);
}

FString SearchAssets(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Query = Args.IsValid() ? MakeShared<FJsonObject>(*Args.Get()) : MakeShared<FJsonObject>();
	FString QueryText;
	if (Query->TryGetStringField(TEXT("query"), QueryText) && !QueryText.IsEmpty() && !Query->HasField(TEXT("searchTerm")))
	{
		Query->SetStringField(TEXT("searchTerm"), QueryText);
	}
	return WorldDataMCP::Tools::FindAssets(Query);
}

FString FindStaticMeshes(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Query = Args.IsValid() ? MakeShared<FJsonObject>(*Args.Get()) : MakeShared<FJsonObject>();
	FString QueryText;
	if (Query->TryGetStringField(TEXT("query"), QueryText) && !QueryText.IsEmpty() && !Query->HasField(TEXT("searchTerm")))
	{
		Query->SetStringField(TEXT("searchTerm"), QueryText);
	}
	Query->SetStringField(TEXT("classFilter"), TEXT("StaticMesh"));
	return WorldDataMCP::Tools::FindAssets(Query);
}

FString GetLevelActors(const TSharedPtr<FJsonObject>& Args)
{
	return WorldDataMCP::Tools::ListLevelActors(Args.IsValid() ? Args : MakeShared<FJsonObject>());
}

FString GetProjectInfo(const TSharedPtr<FJsonObject>& Args)
{
	return FWorldDataMCPServer::GetProjectInfoJson();
}

FString ListProjectModules(const TSharedPtr<FJsonObject>& Args)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);

	const FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source")));
	TArray<FString> BuildFiles;
	IFileManager::Get().FindFilesRecursive(BuildFiles, *SourceRoot, TEXT("*.Build.cs"), true, false);
	BuildFiles.Sort();

	TArray<TSharedPtr<FJsonValue>> Modules;
	for (const FString& File : BuildFiles)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), FPaths::GetBaseFilename(File).Replace(TEXT(".Build"), TEXT("")));
		Row->SetStringField(TEXT("buildFile"), MakeProjectRelative(File));
		Modules.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetNumberField(TEXT("moduleCount"), Modules.Num());
	Result->SetArrayField(TEXT("modules"), Modules);
	return SerializeObject(Result);
}

FString GetBuildConfiguration(const TSharedPtr<FJsonObject>& Args)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
#if UE_BUILD_DEBUG
	Result->SetStringField(TEXT("configuration"), TEXT("Debug"));
#elif UE_BUILD_DEVELOPMENT
	Result->SetStringField(TEXT("configuration"), TEXT("Development"));
#elif UE_BUILD_SHIPPING
	Result->SetStringField(TEXT("configuration"), TEXT("Shipping"));
#else
	Result->SetStringField(TEXT("configuration"), TEXT("Unknown"));
#endif
	Result->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());
	Result->SetBoolField(TEXT("withEditor"), GIsEditor);
	Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	return SerializeObject(Result);
}

FString ReadFile(const TSharedPtr<FJsonObject>& Args)
{
	FString FullPath;
	FString Error;
	if (!ResolveProjectFilePath(GetStringField(Args, TEXT("file_path")), FullPath, Error))
	{
		return ErrorJson(Error);
	}
	if (!ValidateProjectTextFilePath(FullPath, TEXT("read"), Error))
	{
		return ErrorJson(Error);
	}
	if (!IFileManager::Get().FileExists(*FullPath))
	{
		return ErrorJson(FString::Printf(TEXT("File does not exist: %s"), *FullPath));
	}
	if (IFileManager::Get().FileSize(*FullPath) > MaxFileReadBytes)
	{
		return ErrorJson(FString::Printf(TEXT("File is larger than the %d byte read limit."), MaxFileReadBytes));
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FullPath))
	{
		return ErrorJson(FString::Printf(TEXT("Failed to read file: %s"), *FullPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("file"), MakeProjectRelative(FullPath));
	Result->SetStringField(TEXT("content"), Content);
	return SerializeObject(Result);
}

FString WriteFile(const TSharedPtr<FJsonObject>& Args)
{
	FString FullPath;
	FString Error;
	if (!ResolveProjectFilePath(GetStringField(Args, TEXT("file_path")), FullPath, Error))
	{
		return ErrorJson(Error);
	}
	if (!ValidateProjectTextFilePath(FullPath, TEXT("write"), Error))
	{
		return ErrorJson(Error);
	}

	const FString Content = GetStringField(Args, TEXT("content"));
	if (Content.Len() > MaxFileWriteChars)
	{
		return ErrorJson(FString::Printf(TEXT("Content is larger than the %d character write limit."), MaxFileWriteChars));
	}
	if (!WriteStringAtomically(Content, FullPath))
	{
		return ErrorJson(FString::Printf(TEXT("Failed to write file: %s"), *FullPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("file"), MakeProjectRelative(FullPath));
	Result->SetNumberField(TEXT("bytes"), Content.Len());
	Result->SetNumberField(TEXT("chars"), Content.Len());
	return SerializeObject(Result);
}

FString DeleteFile(const TSharedPtr<FJsonObject>& Args)
{
	FString FullPath;
	FString Error;
	if (!ResolveProjectFilePath(GetStringField(Args, TEXT("file_path")), FullPath, Error))
	{
		return ErrorJson(Error);
	}
	if (!ValidateProjectTextFilePath(FullPath, TEXT("delete"), Error))
	{
		return ErrorJson(Error);
	}
	if (!IFileManager::Get().FileExists(*FullPath))
	{
		return ErrorJson(FString::Printf(TEXT("File does not exist: %s"), *FullPath));
	}
	if (!IFileManager::Get().Delete(*FullPath, false, true, true))
	{
		return ErrorJson(FString::Printf(TEXT("Failed to delete file: %s"), *FullPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("file"), MakeProjectRelative(FullPath));
	return SerializeObject(Result);
}

FString RenameFile(const TSharedPtr<FJsonObject>& Args)
{
	FString OldPath;
	FString NewPath;
	FString Error;
	if (!ResolveProjectFilePath(GetStringField(Args, TEXT("old_path")), OldPath, Error))
	{
		return ErrorJson(Error);
	}
	if (!ValidateProjectTextFilePath(OldPath, TEXT("rename"), Error))
	{
		return ErrorJson(Error);
	}
	if (!ResolveProjectFilePath(GetStringField(Args, TEXT("new_path")), NewPath, Error))
	{
		return ErrorJson(Error);
	}
	if (!ValidateProjectTextFilePath(NewPath, TEXT("rename"), Error))
	{
		return ErrorJson(Error);
	}
	if (!IFileManager::Get().FileExists(*OldPath))
	{
		return ErrorJson(FString::Printf(TEXT("Source file does not exist: %s"), *OldPath));
	}
	if (IFileManager::Get().FileExists(*NewPath))
	{
		return ErrorJson(FString::Printf(TEXT("Destination file already exists: %s"), *NewPath));
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(NewPath), true);
	if (!IFileManager::Get().Move(*NewPath, *OldPath, false, true))
	{
		return ErrorJson(FString::Printf(TEXT("Failed to move file: %s -> %s"), *OldPath, *NewPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("oldFile"), MakeProjectRelative(OldPath));
	Result->SetStringField(TEXT("newFile"), MakeProjectRelative(NewPath));
	return SerializeObject(Result);
}

FString PlayInEditor(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return ErrorJson(TEXT("No editor is available."));
	}
	if (GEditor->PlayWorld)
	{
		return ErrorJson(TEXT("PIE is already running."));
	}

	FRequestPlaySessionParams Params;
	GEditor->RequestPlaySession(Params);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("message"), TEXT("Play in Editor started."));
	return SerializeObject(Result);
}

FString StopPIE(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return ErrorJson(TEXT("No editor is available."));
	}
	if (!GEditor->PlayWorld)
	{
		return ErrorJson(TEXT("PIE is not running."));
	}

	GEditor->RequestEndPlayMap();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("message"), TEXT("Play in Editor stopped."));
	return SerializeObject(Result);
}

FString PcgRecipeLibraryStatus(const TSharedPtr<FJsonObject>& Args)
{
	TArray<FString> Roots;
	FindRecipeRoots(Roots);
	const FString ActiveRoot = GetActiveRecipeRoot();

	TArray<FString> Files;
	FindJsonFilesRecursive(ActiveRoot, Files);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("activeRoot"), ActiveRoot);
	Result->SetBoolField(TEXT("exists"), IFileManager::Get().DirectoryExists(*ActiveRoot));
	Result->SetNumberField(TEXT("jsonFileCount"), Files.Num());
	AddStringArray(Result, TEXT("candidateRoots"), Roots);

	TArray<FString> RelativeFiles;
	for (int32 Index = 0; Index < Files.Num() && Index < 50; ++Index)
	{
		RelativeFiles.Add(MakeProjectRelative(Files[Index]));
	}
	AddStringArray(Result, TEXT("sampleFiles"), RelativeFiles);

	Result->SetStringField(TEXT("guidance"), TEXT("Use search_pcg_recipes first, then read_pcg_recipe for the selected id/file."));
	return SerializeObject(Result);
}

FString SearchPcgRecipes(const TSharedPtr<FJsonObject>& Args)
{
	const FString Query = GetStringField(Args, TEXT("query")).ToLower();
	TArray<FString> Tags;
	TArray<FString> RequiredSceneInputs;
	TArray<FString> OutputLayers;
	GetStringArrayField(Args, TEXT("tags"), Tags);
	GetStringArrayField(Args, TEXT("required_scene_inputs"), RequiredSceneInputs);
	GetStringArrayField(Args, TEXT("output_layers"), OutputLayers);

	const int32 Limit = FMath::Clamp(static_cast<int32>(GetNumberField(Args, TEXT("limit"), 10)), 1, 100);
	const bool bIncludeRecipe = GetBoolField(Args, TEXT("include_recipe"), false);

	TArray<FString> Files;
	FindJsonFilesRecursive(GetActiveRecipeRoot(), Files);

	TArray<TSharedPtr<FJsonValue>> Hits;
	for (const FString& File : Files)
	{
		if (Hits.Num() >= Limit)
		{
			break;
		}

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *File))
		{
			continue;
		}

		const FString LowerContent = Content.ToLower();
		bool bMatch = Query.IsEmpty() || LowerContent.Contains(Query);
		for (const FString& Tag : Tags)
		{
			bMatch = bMatch && LowerContent.Contains(Tag.ToLower());
		}
		for (const FString& Input : RequiredSceneInputs)
		{
			bMatch = bMatch && LowerContent.Contains(Input.ToLower());
		}
		for (const FString& Layer : OutputLayers)
		{
			bMatch = bMatch && LowerContent.Contains(Layer.ToLower());
		}
		if (!bMatch)
		{
			continue;
		}

		TSharedRef<FJsonObject> Hit = MakeShared<FJsonObject>();
		Hit->SetStringField(TEXT("file"), MakeProjectRelative(File));

		TSharedPtr<FJsonObject> Object = ParseJsonObject(Content);
		if (Object.IsValid())
		{
			for (const TCHAR* FieldName : { TEXT("id"), TEXT("recipe_id"), TEXT("name"), TEXT("title") })
			{
				FString Value;
				if (Object->TryGetStringField(FieldName, Value))
				{
					Hit->SetStringField(FieldName, Value);
				}
			}
			if (bIncludeRecipe)
			{
				Hit->SetObjectField(TEXT("recipe"), Object);
			}
		}
		Hits.Add(MakeShared<FJsonValueObject>(Hit));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("activeRoot"), GetActiveRecipeRoot());
	Result->SetNumberField(TEXT("count"), Hits.Num());
	Result->SetArrayField(TEXT("recipes"), Hits);
	return SerializeObject(Result);
}

FString ReadPcgRecipe(const TSharedPtr<FJsonObject>& Args)
{
	const FString FileArg = GetStringField(Args, TEXT("file"));
	const FString Id = !GetStringField(Args, TEXT("id")).IsEmpty()
		? GetStringField(Args, TEXT("id"))
		: GetStringField(Args, TEXT("recipe_id"));

	FString FullPath;
	FString Error;
	const FString ActiveRoot = GetActiveRecipeRoot();
	if (!FileArg.IsEmpty())
	{
		if (!ResolveRecipeJsonFilePath(FileArg, ActiveRoot, FullPath, Error))
		{
			return ErrorJson(Error);
		}
	}
	else
	{
		TArray<FString> Files;
		FindJsonFilesRecursive(ActiveRoot, Files);
		FullPath = FindJsonFileByIdOrName(Id, Files);
	}

	if (FullPath.IsEmpty() || !IFileManager::Get().FileExists(*FullPath) || !ValidateRecipeJsonFilePath(FullPath, ActiveRoot, Error))
	{
		return ErrorJson(Error.IsEmpty() ? TEXT("PCG recipe was not found.") : Error);
	}
	return ReadJsonFileAsResult(FullPath, TEXT("recipe"));
}

FString ReadPcgSceneBinding(const TSharedPtr<FJsonObject>& Args)
{
	const FString FileArg = GetStringField(Args, TEXT("file"));
	const FString Wanted = !GetStringField(Args, TEXT("binding_id")).IsEmpty()
		? GetStringField(Args, TEXT("binding_id"))
		: GetStringField(Args, TEXT("recipe_id"));

	FString FullPath;
	FString Error;
	const FString ActiveRoot = GetActiveRecipeRoot();
	if (!FileArg.IsEmpty())
	{
		if (!ResolveRecipeJsonFilePath(FileArg, ActiveRoot, FullPath, Error))
		{
			return ErrorJson(Error);
		}
	}
	else
	{
		TArray<FString> Files;
		FindJsonFilesRecursive(ActiveRoot, Files);
		FullPath = FindJsonFileByIdOrName(Wanted, Files);
	}

	if (FullPath.IsEmpty() || !IFileManager::Get().FileExists(*FullPath) || !ValidateRecipeJsonFilePath(FullPath, ActiveRoot, Error))
	{
		return ErrorJson(Error.IsEmpty() ? TEXT("PCG scene binding was not found.") : Error);
	}
	return ReadJsonFileAsResult(FullPath, TEXT("sceneBinding"));
}
}
}
