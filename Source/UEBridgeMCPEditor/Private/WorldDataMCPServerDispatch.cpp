#include "WorldDataMCPServer.h"
#include "WorldDataMCPServerInternal.h"
#include "WorldDataMCPCommon.h"
#include "WorldDataMCPTools.h"
#include "UEBridgeMCPExtractedTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using namespace WorldDataMCP;
using namespace WorldDataMCP::ServerInternal;

FString FWorldDataMCPServer::GetToolDefinitionsJson()
{
	static const FString LocalToolsJson = TEXT(R"JSON([
{"name":"get_current_project_info","description":"Return the UE project identity and MCP endpoint for this editor session.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Project Info","readOnlyHint":true,"openWorldHint":false}},
{"name":"list_level_actors","description":"List actors in the currently loaded editor world with transforms, folder, mobility, attachment parent, and selection state. For scene LAYOUT prefer describe_scene / capture_scene_map; use this for filtered enumeration.","inputSchema":{"type":"object","properties":{"classFilter":{"type":"string","description":"Optional case-insensitive class-name substring."},"nameContains":{"type":"string","description":"Optional case-insensitive actor name or label substring."},"selectedOnly":{"type":"boolean","description":"When true, only return currently selected actors."},"includeBounds":{"type":"boolean","description":"Add each actor's world-space bounds {min,max,center,size}. Default false."},"groupByFolder":{"type":"boolean","description":"Also return a 'folders' histogram of matched actors by outliner folder. Default false."},"maxResults":{"type":"number","description":"Maximum returned actors. Default 200, capped at 1000."},"offset":{"type":"number","description":"Number of matches to skip for paging. Use nextOffset from the previous response."}}},"annotations":{"title":"List Level Actors","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_selected_actors","description":"Return the actors currently selected in the editor viewport/outliner.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Selected Actors","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_actor_details","description":"Read one actor in depth: transform, world-space bounds, tags, attachment parent/children, and its components with classes and relative transforms.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."}},"required":["name"]},"annotations":{"title":"Get Actor Details","readOnlyHint":true,"openWorldHint":false}},
{"name":"find_assets","description":"Search assets under a content path without loading them.","inputSchema":{"type":"object","properties":{"searchTerm":{"type":"string","description":"Optional case-insensitive asset name or path substring."},"classFilter":{"type":"string","description":"Optional class-name substring such as StaticMesh, Blueprint, World, Material."},"path":{"type":"string","description":"Content root to search. Default /Game."},"maxResults":{"type":"number","description":"Maximum returned assets. Default 50, capped at 500."},"offset":{"type":"number","description":"Number of matches to skip for paging. Use nextOffset from the previous response."}}},"annotations":{"title":"Find Assets","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_asset","description":"Read basic Asset Registry metadata for one asset.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Asset object path or package path, for example /Game/Foo/Bar.Bar or /Game/Foo/Bar."},"path":{"type":"string","description":"Alias for assetPath."}},"required":["assetPath"]},"annotations":{"title":"Read Asset","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_content_summary","description":"Summarize the Asset Registry under a content path: total asset count and a histogram of asset counts by class.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Content root to summarize. Default /Game."},"maxClasses":{"type":"number","description":"Maximum class buckets returned. Default 30, capped at 200."}}},"annotations":{"title":"Get Content Summary","readOnlyHint":true,"openWorldHint":false}},
{"name":"analyze_scene_performance","description":"Static performance estimate of the current editor world from LOD0 render data: actor/component counts, total triangles, estimated draw calls, instancing, Nanite, lights by mobility (with movable shadow-casters), unique/translucent materials, the heaviest actors by triangle count, and heuristic warnings. Not a GPU profile.","inputSchema":{"type":"object","properties":{"topActors":{"type":"number","description":"Number of heaviest actors (by LOD0 triangle count) to return. Default 10, capped at 50. Use 0 to omit the list."},"heavyTriangleThreshold":{"type":"number","description":"LOD0 triangle count above which an actor is flagged in warnings. Default 500000."}}},"annotations":{"title":"Analyze Scene Performance","readOnlyHint":true,"openWorldHint":false}},
{"name":"select_actor","description":"Select an actor in the editor by name or label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."}}},"annotations":{"title":"Select Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"spawn_actor","description":"Spawn an actor into the current editor world. Use staticMeshPath to create a StaticMeshActor.","inputSchema":{"type":"object","properties":{"class":{"type":"string","description":"Actor class name/path. Default Actor. Blueprint asset paths are supported."},"staticMeshPath":{"type":"string","description":"Optional StaticMesh asset path. When supplied, spawns a StaticMeshActor."},"label":{"type":"string","description":"Optional editor label."},"location":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."},"rotation":{"type":"object","description":"Object {pitch,yaw,roll} or array [pitch,yaw,roll]."},"scale":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."}}},"annotations":{"title":"Spawn Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
{"name":"get_codex_policy_snapshot","description":"Read a redacted snapshot of explicit local Codex config policy such as approval_policy, sandbox_mode, active profile, model, and MCP server blocks. Does not expose hidden prompts or env secrets.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Codex Policy Snapshot","readOnlyHint":true,"openWorldHint":false}},
)JSON")
		TEXT(R"JSON({"name":"get_object_properties","description":"Read reflected UPROPERTY values of an actor (or one of its components) via engine reflection. By default returns only editable properties that DIFFER from the archetype/default (an absent property is at its default); pass includeDefaults=true for all, or 'properties' for specific names.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."},"component":{"type":"string","description":"Optional component name to target instead of the actor."},"properties":{"type":"array","items":{"type":"string"},"description":"Optional specific property names. When omitted, returns editable non-default properties."},"includeDefaults":{"type":"boolean","description":"Include properties that match their default value. Default false."},"maxProperties":{"type":"number","description":"Maximum properties returned. Default 100, capped at 500."},"maxBytes":{"type":"number","description":"Approximate serialized byte budget for the values. Default 16384."}},"required":["name"]},"annotations":{"title":"Get Object Properties","readOnlyHint":true,"openWorldHint":false}},
{"name":"set_object_property","description":"Set one reflected UPROPERTY on an actor or component via reflection, inside a transaction with editor change notifications. Object/class reference properties (e.g. StaticMesh, Material, a class ref) accept an asset/class path string, or null to clear; other types use JSON matching the property's type.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string"},"component":{"type":"string","description":"Optional component name to target."},"property":{"type":"string","description":"Property name or path: nested struct members, array indices, and map keys, e.g. 'RelativeLocation.X', 'OverrideMaterials[0]', or 'MyMap[\"key\"]'."},"value":{"description":"New value. For object/class reference properties, an asset/class path string (or null to clear); otherwise JSON matching the property's type."},"force":{"type":"boolean","description":"Override the editable-property safety check (allow setting read-only/transient/instance-locked properties). Default false."}},"required":["name","property","value"]},"annotations":{"title":"Set Object Property","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_actor_transform","description":"Set location/rotation/scale of an existing actor. Provide any subset; omitted components are left unchanged.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string"},"location":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."},"rotation":{"type":"object","description":"Object {pitch,yaw,roll} or array [pitch,yaw,roll]."},"scale":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."}},"required":["name"]},"annotations":{"title":"Set Actor Transform","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"delete_actor","description":"Delete an actor from the current editor world by name or label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string"}},"required":["name"]},"annotations":{"title":"Delete Actor","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"capture_viewport","description":"Capture the active editor viewport. Returns an inline PNG image (so a vision-capable agent can see the scene) plus a saved file path and dimensions.","inputSchema":{"type":"object","properties":{"maxWidth":{"type":"number","description":"Downscale so width <= this, preserving aspect. Default 1280; 0 keeps full resolution."},"inline":{"type":"boolean","description":"Embed the PNG inline in the response so the agent can see it. Default true."}}},"annotations":{"title":"Capture Viewport","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_component","description":"Add a component (by class) to an existing actor as an instance component. Scene components attach to the actor root.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string"},"componentClass":{"type":"string","description":"Component class name/path, e.g. StaticMeshComponent or PointLightComponent."},"class":{"type":"string","description":"Alias for componentClass."},"componentName":{"type":"string","description":"Optional desired component name."}},"required":["name","componentClass"]},"annotations":{"title":"Add Component","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"attach_actor","description":"Attach an actor to a parent actor, or detach it (detach=true). Parent is required unless detaching.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Child actor name or label."},"child":{"type":"string","description":"Alias for name."},"label":{"type":"string","description":"Alias for name."},"parent":{"type":"string","description":"Parent actor name or label."},"socket":{"type":"string","description":"Optional parent socket/bone name."},"keepWorldTransform":{"type":"boolean","description":"Keep world transform on attach. Default true."},"detach":{"type":"boolean","description":"Detach from current parent instead of attaching."}},"required":["name"]},"annotations":{"title":"Attach Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"duplicate_actor","description":"Duplicate an existing actor (copies properties and components). Optionally place it and give it a new label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Source actor name or label."},"label":{"type":"string"},"location":{"type":"object","description":"Optional placement {x,y,z} or [x,y,z]; defaults to the source location."},"rotation":{"type":"object","description":"Optional rotation {pitch,yaw,roll} or [pitch,yaw,roll]."},"newLabel":{"type":"string","description":"Optional label for the duplicate."}},"required":["name"]},"annotations":{"title":"Duplicate Actor","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
)JSON")
		TEXT(R"JSON(,
{"name":"set_scene_brief","description":"REQUIRED before any scene generation. Records the design CONCEPT BRIEF for the current level and UNLOCKS the generation tools (place_meshes, lay_meshes_along_spline, add_foliage_instances, spawn_actor-with-mesh, apply_native_pcg_rule) which otherwise refuse to run. All fields required; language must be 'formal' or 'informal' (pick ONE compositional language, never both). A brief authorizes generation only for the level it was set in.","inputSchema":{"type":"object","properties":{"oneIdea":{"type":"string","description":"Single-sentence concept, e.g. 'a quiet formal memorial garden at golden hour'."},"language":{"type":"string","enum":["formal","informal"],"description":"The ONE compositional language: formal (axial/symmetry) or informal (naturalistic/clustered)."},"hero":{"type":"string","description":"The focal point and how it is framed."},"palette":{"type":"string","description":"2-3 species/materials, one dominant."},"atmosphere":{"type":"string","description":"Time of day + sun angle + color-grade target."},"boundary":{"type":"string","description":"How the space is contained + where the entry/threshold is."}},"required":["oneIdea","language","hero","palette","atmosphere","boundary"]},"annotations":{"title":"Set Scene Brief","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"get_scene_brief","description":"Return the active scene brief (if any) and whether scene generation is currently unlocked for the current level.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Scene Brief","readOnlyHint":true,"openWorldHint":false}},
{"name":"clear_scene_brief","description":"Clear the active scene brief, re-gating scene generation. Use when starting a different scene.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Clear Scene Brief","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}}
])JSON");
	return BuildCombinedToolDefinitionsJson(LocalToolsJson);
}

FString FWorldDataMCPServer::GetResourceListJson()
{
	// Delegate to the unified canonical catalog so the MCP resources/list method and the
	// list_resources tool always advertise the exact same resource set (one source of truth).
	return WorldDataMCP::ExtractedTools::ListResources();
}

FString FWorldDataMCPServer::DispatchTool(const FString& ToolName, const FString& ArgsJson, bool bTrustedToolAccess)
{
	if (!bTrustedToolAccess && ToolRequiresTrustedClient(ToolName))
	{
		return ErrorJson(FString::Printf(TEXT("Tool '%s' requires a trusted MCP client."), *ToolName));
	}

	TSharedPtr<FJsonObject> Args = ParseJsonObject(ArgsJson);
	if (!Args.IsValid())
	{
		return ErrorJson(TEXT("Invalid arguments JSON."));
	}

	// Built-in tools implemented directly on the server. Looked up by name instead of a
	// long if-chain; each entry wraps the concrete handler so the signatures stay uniform.
	using FLocalToolHandler = TFunction<FString(const TSharedPtr<FJsonObject>&)>;
	static const TMap<FString, FLocalToolHandler> LocalTools =
	{
		{ TEXT("get_current_project_info"),  [](const TSharedPtr<FJsonObject>&)  { return FWorldDataMCPServer::GetProjectInfoJson(); } },
		{ TEXT("list_level_actors"),         [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::ListLevelActors(A); } },
		{ TEXT("get_selected_actors"),       [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::GetSelectedActors(A); } },
		{ TEXT("get_actor_details"),         [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::GetActorDetails(A); } },
		{ TEXT("find_assets"),               [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::FindAssets(A); } },
		{ TEXT("get_content_summary"),       [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::GetContentSummary(A); } },
		{ TEXT("analyze_scene_performance"), [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::AnalyzeScenePerformance(A); } },
		{ TEXT("read_asset"),                [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::ReadAsset(A); } },
		{ TEXT("select_actor"),              [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::SelectActor(A); } },
		{ TEXT("spawn_actor"),               [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::SpawnActor(A); } },
		{ TEXT("get_object_properties"),     [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::GetObjectProperties(A); } },
		{ TEXT("set_object_property"),       [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::SetObjectProperty(A); } },
		{ TEXT("set_actor_transform"),       [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::SetActorTransform(A); } },
		{ TEXT("delete_actor"),              [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::DeleteActor(A); } },
		{ TEXT("add_component"),             [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::AddComponent(A); } },
		{ TEXT("attach_actor"),              [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::AttachActor(A); } },
		{ TEXT("duplicate_actor"),           [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::DuplicateActor(A); } },
			{ TEXT("set_scene_brief"),           [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::SetSceneBrief(A); } },
			{ TEXT("get_scene_brief"),           [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::GetSceneBrief(A); } },
			{ TEXT("clear_scene_brief"),         [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::ClearSceneBrief(A); } },
		{ TEXT("capture_viewport"),          [](const TSharedPtr<FJsonObject>& A) { return WorldDataMCP::Tools::CaptureViewport(A); } },
		{ TEXT("get_codex_policy_snapshot"), [](const TSharedPtr<FJsonObject>&)  { return GetCodexPolicySnapshotJson(); } },
	};

	if (const FLocalToolHandler* Handler = LocalTools.Find(ToolName))
	{
		return (*Handler)(Args);
	}

	// Fall through to the modular tool registry (one entry per domain module).
	for (const FMCPToolModule& Module : GetMCPToolModules())
	{
		FString ModuleResult;
		if (Module.Dispatch(ToolName, Args, ModuleResult))
		{
			return ModuleResult;
		}
	}

	return ErrorJson(FString::Printf(TEXT("Unknown UEBridgeMCP tool: %s"), *ToolName));
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::MakeJsonRpcError(TSharedPtr<FJsonValue> Id, int32 Code, const FString& Message)
{
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetNumberField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Response->SetObjectField(TEXT("error"), Error);
	return Response;
}
