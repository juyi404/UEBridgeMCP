#include "UEBridgeMCPExtractedTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataMCPServer.h"
#include "WorldDataMCPTools.h"

#include "Algo/Sort.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace WorldDataMCP
{
namespace ExtractedTools
{
namespace
{
	constexpr int32 MaxFileReadBytes = 1024 * 1024;
	constexpr int32 MaxPythonCommandBytes = 64 * 1024;
	constexpr int32 MaxSourceFiles = 400;
	constexpr int32 MaxResourceRows = 300;

	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		return JsonObjectToString(Object);
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& JsonText)
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			return nullptr;
		}
		return Parsed;
	}

	TSharedPtr<FJsonObject> LoadJsonObjectFile(const FString& Path)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *Path))
		{
			return nullptr;
		}
		return ParseObject(Content);
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
		FPaths::NormalizeDirectoryName(ProjectDir);

		FString Candidate = FPaths::IsRelative(InputPath)
			? FPaths::Combine(ProjectDir, InputPath)
			: InputPath;
		Candidate = FPaths::ConvertRelativePathToFull(Candidate);
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

	FString NormalizeDirectoryForContainment(FString Directory)
	{
		Directory = FPaths::ConvertRelativePathToFull(Directory);
		FPaths::NormalizeDirectoryName(Directory);
		return Directory;
	}

	FString NormalizeFileForContainment(FString File)
	{
		File = FPaths::ConvertRelativePathToFull(File);
		FPaths::NormalizeFilename(File);
		return File;
	}

	bool IsPathInsideDirectory(const FString& File, const FString& Directory)
	{
		const FString NormalizedFile = NormalizeFileForContainment(File);
		const FString NormalizedDirectory = NormalizeDirectoryForContainment(Directory);
		return NormalizedFile.StartsWith(NormalizedDirectory + TEXT("/"), ESearchCase::IgnoreCase);
	}

	bool ValidateReadableJsonFileWithinRoot(const FString& CandidatePath, const FString& AllowedRoot, FString& OutPath, FString& OutError)
	{
		if (CandidatePath.IsEmpty())
		{
			OutError = TEXT("File path is empty.");
			return false;
		}

		const FString NormalizedFile = NormalizeFileForContainment(CandidatePath);
		if (!IsPathInsideDirectory(NormalizedFile, AllowedRoot))
		{
			OutError = FString::Printf(TEXT("Path is outside the allowed PCG recipe root: %s"), *MakeProjectRelative(NormalizedFile));
			return false;
		}
		if (!FPaths::GetExtension(NormalizedFile, false).Equals(TEXT("json"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("Only .json PCG recipe files can be read.");
			return false;
		}
		if (!IFileManager::Get().FileExists(*NormalizedFile))
		{
			OutError = FString::Printf(TEXT("File does not exist: %s"), *MakeProjectRelative(NormalizedFile));
			return false;
		}

		const int64 FileSize = IFileManager::Get().FileSize(*NormalizedFile);
		if (FileSize < 0)
		{
			OutError = FString::Printf(TEXT("Unable to stat file: %s"), *MakeProjectRelative(NormalizedFile));
			return false;
		}
		if (FileSize > MaxFileReadBytes)
		{
			OutError = FString::Printf(TEXT("File is larger than the %d byte read limit."), MaxFileReadBytes);
			return false;
		}

		OutPath = NormalizedFile;
		return true;
	}

	bool ResolvePcgJsonFilePath(const FString& FileArg, const FString& FallbackRoot, const FString& AllowedRoot, FString& OutPath, FString& OutError)
	{
		if (FileArg.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("file is required.");
			return false;
		}

		FString Candidate;
		FString ProjectPathError;
		if (!ResolveProjectFilePath(FileArg, Candidate, ProjectPathError))
		{
			Candidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(FallbackRoot, FileArg));
		}

		return ValidateReadableJsonFileWithinRoot(Candidate, AllowedRoot, OutPath, OutError);
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

	FString GetBootstrapResource()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("source"), TEXT("UEBridgeMCP extracted tools"));
		Result->SetStringField(TEXT("purpose"), TEXT("Read this first for compact Unreal project context."));
		Result->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : GWorld;
		if (World)
		{
			Result->SetStringField(TEXT("levelName"), World->GetMapName());
		}
		USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;
		Result->SetNumberField(TEXT("selectedActorCount"), Selection ? Selection->Num() : 0);

		TArray<TSharedPtr<FJsonValue>> ReadOrder;
		auto AddStep = [&ReadOrder](const FString& Uri, const FString& Why)
		{
			TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("uri"), Uri);
			Step->SetStringField(TEXT("why"), Why);
			ReadOrder.Add(MakeShared<FJsonValueObject>(Step));
		};
		AddStep(TEXT("ubridge://project/info"), TEXT("Basic project identity and paths."));
		AddStep(TEXT("ubridge://project/plugins"), TEXT("Enabled plugin inventory."));
		AddStep(TEXT("ubridge://project/source-index"), TEXT("Project source file index."));
		AddStep(TEXT("ubridge://content/assets"), TEXT("Asset registry overview."));
		AddStep(TEXT("ubridge://level/current"), TEXT("Current level summary."));
		AddStep(TEXT("ubridge://level/actors"), TEXT("Detailed actor context."));
		AddStep(TEXT("ubridge://blueprints/index"), TEXT("Blueprint inventory."));
		AddStep(TEXT("ubridge://pcg/graphs"), TEXT("PCG graph inventory."));
		AddStep(TEXT("ubridge://editor/problems"), TEXT("Recent warnings and errors."));
		AddStep(TEXT("ubridge://editor/selection"), TEXT("Focused selection context."));
		Result->SetArrayField(TEXT("recommendedReadOrder"), ReadOrder);

		return SerializeObject(Result);
	}

	void FindRecipeRoots(TArray<FString>& OutRoots)
	{
		const FString UEBridgeRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("pcg_tool"), TEXT("recipe_library")));
		OutRoots.Add(UEBridgeRoot);
		OutRoots.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Nwiro"), TEXT("pcg_tool"), TEXT("recipe_library"))));
		OutRoots.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PCGRecipeLibrary"))));
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
		if (!FPaths::GetExtension(File, false).Equals(TEXT("json"), ESearchCase::IgnoreCase))
		{
			return ErrorJson(TEXT("Only .json files can be read by this helper."));
		}
		const int64 FileSize = IFileManager::Get().FileSize(*File);
		if (FileSize < 0)
		{
			return ErrorJson(FString::Printf(TEXT("Unable to stat file: %s"), *File));
		}
		if (FileSize > MaxFileReadBytes)
		{
			return ErrorJson(FString::Printf(TEXT("File is larger than the %d byte read limit."), MaxFileReadBytes));
		}

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *File))
		{
			return ErrorJson(FString::Printf(TEXT("Failed to read file: %s"), *File));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("file"), MakeProjectRelative(File));

		TSharedPtr<FJsonObject> Object = ParseObject(Content);
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
{"name":"list_resources","description":"List UEBridgeMCP standalone resources using ubridge:// URIs.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"List Resources","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_resource","description":"Read a UEBridgeMCP standalone resource by URI. Recommended first read: ubridge://context/bootstrap.","inputSchema":{"type":"object","properties":{"uri":{"type":"string"}},"required":["uri"]},"annotations":{"title":"Read Resource","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_log","description":"Read recent Unreal log lines from the project Saved/Logs folder. Supports lines, severity, and category filters.","inputSchema":{"type":"object","properties":{"lines":{"type":"number","description":"Number of lines. Default 50."},"severity":{"type":"string","description":"Filter: Error, Warning, Log."},"category":{"type":"string"}}},"annotations":{"title":"Read Log","readOnlyHint":true,"openWorldHint":false}},
{"name":"execute_python","description":"Execute Python code in the Unreal Editor through PythonScriptPlugin. Requires unsafe_confirm exactly equal to 'I understand this runs arbitrary Unreal Python'. Prefer structured UE tools first.","inputSchema":{"type":"object","properties":{"code":{"type":"string","description":"Python code to execute."},"unsafe_confirm":{"type":"string","description":"Must equal: I understand this runs arbitrary Unreal Python"}},"required":["code","unsafe_confirm"]},"annotations":{"title":"Execute Python","readOnlyHint":false,"destructiveHint":true,"openWorldHint":true}},
{"name":"search_assets","description":"Search project assets by name, path, and optional class filter.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"searchTerm":{"type":"string"},"classFilter":{"type":"string"},"path":{"type":"string"},"maxResults":{"type":"number"}}},"annotations":{"title":"Search Assets","readOnlyHint":true,"openWorldHint":false}},
{"name":"find_static_meshes","description":"Search StaticMesh assets for placement or PCG use.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"searchTerm":{"type":"string"},"path":{"type":"string"},"maxResults":{"type":"number"}}},"annotations":{"title":"Find Static Meshes","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_level_actors","description":"UEBridgeMCP alias for listing current editor level actors.","inputSchema":{"type":"object","properties":{"classFilter":{"type":"string"},"nameContains":{"type":"string"},"selectedOnly":{"type":"boolean"},"maxResults":{"type":"number"}}},"annotations":{"title":"Get Level Actors","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_project_info","description":"Get project name, engine version, paths, and active MCP endpoint.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Project Info","readOnlyHint":true,"openWorldHint":false}},
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
	if (ToolName == TEXT("read_log")) { OutResult = ReadLog(Args); return true; }
	if (ToolName == TEXT("execute_python")) { OutResult = ExecutePython(Args); return true; }
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
	TArray<TSharedPtr<FJsonValue>> Resources;
	AddResource(Resources, TEXT("ubridge://context/bootstrap"), TEXT("Bootstrap Context"), TEXT("Recommended first-read order and compact editor state."));
	AddResource(Resources, TEXT("ubridge://project/info"), TEXT("Project Info"), TEXT("Engine version, project name, paths, and MCP endpoint."));
	AddResource(Resources, TEXT("ubridge://project/plugins"), TEXT("Project Plugins"), TEXT("Enabled plugin inventory and plugin metadata."));
	AddResource(Resources, TEXT("ubridge://project/source-index"), TEXT("Project Source Index"), TEXT("Source and plugin code file index."));
	AddResource(Resources, TEXT("ubridge://content/assets"), TEXT("Content Assets"), TEXT("Asset registry survey under /Game."));
	AddResource(Resources, TEXT("ubridge://level/current"), TEXT("Current Level"), TEXT("Current map summary and actor class distribution."));
	AddResource(Resources, TEXT("ubridge://level/actors"), TEXT("Level Actors"), TEXT("Current editor-world actors and transforms."));
	AddResource(Resources, TEXT("ubridge://level/components"), TEXT("Level Components"), TEXT("Component class distribution in the current world."));
	AddResource(Resources, TEXT("ubridge://blueprints/index"), TEXT("Blueprint Index"), TEXT("Blueprint asset inventory."));
	AddResource(Resources, TEXT("ubridge://pcg/graphs"), TEXT("PCG Graphs"), TEXT("PCG graph asset inventory."));
	AddResource(Resources, TEXT("ubridge://editor/problems"), TEXT("Editor Problems"), TEXT("Recent warning/error log lines."));
	AddResource(Resources, TEXT("ubridge://editor/selection"), TEXT("Editor Selection"), TEXT("Currently selected actors."));
	AddResource(Resources, TEXT("ubridge://editor/performance"), TEXT("Performance Stats"), TEXT("Memory and actor-count snapshot."));
	AddResource(Resources, TEXT("ubridge://editor/log"), TEXT("Editor Log"), TEXT("Recent project log lines."));
	AddResource(Resources, TEXT("ubridge://editor/viewport"), TEXT("Viewport Info"), TEXT("Viewport availability summary."));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("source"), TEXT("UEBridgeMCP"));
	Result->SetStringField(TEXT("recommendedFirstRead"), TEXT("ubridge://context/bootstrap"));
	Result->SetArrayField(TEXT("resources"), Resources);
	return SerializeObject(Result);
}

FString ReadResource(const TSharedPtr<FJsonObject>& Args)
{
	const FString Uri = GetStringField(Args, TEXT("uri"));
	if (Uri.IsEmpty())
	{
		return ErrorJson(TEXT("uri is required."));
	}

	if (Uri == TEXT("ubridge://context/bootstrap")) return GetBootstrapResource();
	if (Uri == TEXT("ubridge://project/info")) return FWorldDataMCPServer::GetProjectInfoJson();
	if (Uri == TEXT("ubridge://project/plugins")) return GetProjectPluginsResource();
	if (Uri == TEXT("ubridge://project/source-index")) return GetProjectSourceIndexResource();
	if (Uri == TEXT("ubridge://content/assets")) return FWorldDataMCPServer::ReadResource(TEXT("worlddata://content/assets"));
	if (Uri == TEXT("ubridge://level/current")) return GetCurrentLevelResource();
	if (Uri == TEXT("ubridge://level/actors")) return FWorldDataMCPServer::ReadResource(TEXT("worlddata://level/actors"));
	if (Uri == TEXT("ubridge://level/components")) return GetLevelComponentsResource();
	if (Uri == TEXT("ubridge://blueprints/index"))
	{
		TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
		Query->SetStringField(TEXT("classFilter"), TEXT("Blueprint"));
		Query->SetNumberField(TEXT("maxResults"), MaxResourceRows);
		return WorldDataMCP::Tools::FindAssets(Query);
	}
	if (Uri == TEXT("ubridge://pcg/graphs"))
	{
		TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
		Query->SetStringField(TEXT("classFilter"), TEXT("PCG"));
		Query->SetNumberField(TEXT("maxResults"), MaxResourceRows);
		return WorldDataMCP::Tools::FindAssets(Query);
	}
	if (Uri == TEXT("ubridge://editor/problems"))
	{
		TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
		Query->SetNumberField(TEXT("lines"), 80);
		return ReadLog(Query);
	}
	if (Uri == TEXT("ubridge://editor/selection")) return FWorldDataMCPServer::ReadResource(TEXT("worlddata://editor/selection"));
	if (Uri == TEXT("ubridge://editor/performance")) return GetEditorPerformanceResource();
	if (Uri == TEXT("ubridge://editor/log")) return ReadLog(MakeShared<FJsonObject>());
	if (Uri == TEXT("ubridge://editor/viewport")) return GetViewportResource();

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

FString ExecutePython(const TSharedPtr<FJsonObject>& Args)
{
	const FString Code = GetStringField(Args, TEXT("code"));
	if (Code.IsEmpty())
	{
		return ErrorJson(TEXT("code is required."));
	}
	if (Code.Len() > MaxPythonCommandBytes)
	{
		return ErrorJson(FString::Printf(TEXT("Python code exceeds the %d character limit."), MaxPythonCommandBytes));
	}

	const FString UnsafeConfirm = GetStringField(Args, TEXT("unsafe_confirm"));
	if (UnsafeConfirm != TEXT("I understand this runs arbitrary Unreal Python"))
	{
		return ErrorJson(TEXT("execute_python requires unsafe_confirm exactly equal to 'I understand this runs arbitrary Unreal Python'. Prefer structured UEBridgeMCP tools when possible."));
	}

	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python)
	{
		return ErrorJson(TEXT("PythonScriptPlugin is not available."));
	}

	const bool bSuccess = Python->ExecPythonCommand(*Code);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(TEXT("message"), bSuccess ? TEXT("Python command executed.") : TEXT("Python command failed."));
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

	const FString Content = GetStringField(Args, TEXT("content"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullPath), true);
	if (!FFileHelper::SaveStringToFile(Content, *FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return ErrorJson(FString::Printf(TEXT("Failed to write file: %s"), *FullPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("file"), MakeProjectRelative(FullPath));
	Result->SetNumberField(TEXT("bytes"), Content.Len());
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
	if (!ResolveProjectFilePath(GetStringField(Args, TEXT("new_path")), NewPath, Error))
	{
		return ErrorJson(Error);
	}
	if (!IFileManager::Get().FileExists(*OldPath))
	{
		return ErrorJson(FString::Printf(TEXT("Source file does not exist: %s"), *OldPath));
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(NewPath), true);
	if (!IFileManager::Get().Move(*NewPath, *OldPath, true, true))
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

		TSharedPtr<FJsonObject> Object = ParseObject(Content);
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
		if (!ResolvePcgJsonFilePath(FileArg, ActiveRoot, ActiveRoot, FullPath, Error))
		{
			return ErrorJson(Error);
		}
	}
	else
	{
		TArray<FString> Files;
		FindJsonFilesRecursive(ActiveRoot, Files);
		FullPath = FindJsonFileByIdOrName(Id, Files);
		if (FullPath.IsEmpty())
		{
			return ErrorJson(TEXT("PCG recipe was not found."));
		}
		if (!ValidateReadableJsonFileWithinRoot(FullPath, ActiveRoot, FullPath, Error))
		{
			return ErrorJson(Error);
		}
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
	const FString PcgRoot = FPaths::GetPath(ActiveRoot);
	if (!FileArg.IsEmpty())
	{
		if (!ResolvePcgJsonFilePath(FileArg, ActiveRoot, ActiveRoot, FullPath, Error))
		{
			return ErrorJson(Error);
		}
	}
	else
	{
		TArray<FString> Files;
		FindJsonFilesRecursive(PcgRoot, Files);
		FullPath = FindJsonFileByIdOrName(Wanted, Files);
		if (FullPath.IsEmpty())
		{
			return ErrorJson(TEXT("PCG scene binding was not found."));
		}
		if (!ValidateReadableJsonFileWithinRoot(FullPath, PcgRoot, FullPath, Error))
		{
			return ErrorJson(Error);
		}
	}

	return ReadJsonFileAsResult(FullPath, TEXT("sceneBinding"));
}
}
}
