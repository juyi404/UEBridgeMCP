#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class AActor;
class FJsonObject;
class FJsonValue;
class UWorld;

// Spatial-perception tools — the layer that turns a flat actor list into an actual
// understanding of WHERE things are and HOW BIG the world is. These are what let an agent
// reason about layout ("a road runs N-S, a forest sits east") instead of staring at a thousand
// raw transforms. Read-only; self-contained like the other tool modules (the server merges
// GetToolDefinitionsJson() and routes via Dispatch()).
//
// The helpers above the tool entry-points are shared: describe_scene and the actor listings
// reuse GetActorWorldBounds / ComputeLevelBounds / BuildRegionSummaries so bounds and clustering
// are computed one way everywhere.
namespace WorldDataMCP
{
namespace SpatialTools
{
	// World-space axis-aligned bounding box of an actor's primitive components.
	// Returns an invalid box (FBox::IsValid == 0) when the actor has no spatial extent.
	UEBRIDGEMCPSPATIALTOOLS_API FBox GetActorWorldBounds(const AActor* Actor);

	// True when the actor contributes meaningful, finite extent to the scene — excludes
	// component-less actors and world-spanning bounds (sky/atmosphere/huge volumes) that would
	// otherwise swallow the whole level extent.
	UEBRIDGEMCPSPATIALTOOLS_API bool IsSpatiallyMeaningfulActor(const AActor* Actor, const FBox& ActorBounds);

	// Union of every spatially-meaningful actor's bounds. OutConsidered counts contributors.
	UEBRIDGEMCPSPATIALTOOLS_API FBox ComputeLevelBounds(UWorld* World, int32& OutConsidered);

	// Compact {min,max,center,size} integer arrays for a box; empty object if the box is invalid.
	UEBRIDGEMCPSPATIALTOOLS_API TSharedPtr<FJsonObject> BoxToJson(const FBox& Box);

	// Coarse semantic bucket used for grouping: light / landscape / foliage / spline / volume /
	// staticMesh / skeletalMesh / camera / actor (fallback). Cheap component/class probes only.
	UEBRIDGEMCPSPATIALTOOLS_API FString ClassifyActor(const AActor* Actor);

	// Grid-cluster the meaningful actors over the level's XY footprint and summarize the densest
	// cells (count, dominant category, category histogram, union bounds, a few sample labels).
	// Returns at most MaxRegions region objects sorted by actor count.
	UEBRIDGEMCPSPATIALTOOLS_API TArray<TSharedPtr<FJsonValue>> BuildRegionSummaries(UWorld* World, const FBox& LevelBounds, int32 GridDivisions, int32 MaxRegions);

	// Visit every instanced-mesh (ISM/HISM) instance in the world as a
	// (owner, instanceIndex, world-AABB, category) unit. This is how the perception layer SEES
	// procedural scatter — PCG-ISM, foliage-HISM, lay-spline ISM — which lives as sub-actor
	// instances the actor-level helpers above are blind to. HISM is caught via its ISM base;
	// category is derived from the owner's semantics + the instanced mesh name.
	UEBRIDGEMCPSPATIALTOOLS_API void ForEachWorldInstance(UWorld* World, TFunctionRef<void(const AActor* Owner, int32 InstanceIndex, const FBox& WorldBounds, const FString& Category)> Visit);

	// Collect the world-space AABBs of every placed unit (StaticMesh actors + all ISM/HISM instances)
	// for generation-time clearance — so an imperative scatter pass can prune against the OTHER routes'
	// output (incl. non-collidable PCG/foliage instances a physics overlap would miss). RegionFilter,
	// when valid, keeps only bounds intersecting it (bounds the cost to the scatter area).
	UEBRIDGEMCPSPATIALTOOLS_API void GatherPlacementBounds(UWorld* World, TArray<FBox>& OutBounds, const FBox& RegionFilter = FBox(ForceInit));

	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
