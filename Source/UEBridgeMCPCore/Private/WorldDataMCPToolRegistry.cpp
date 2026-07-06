#include "WorldDataMCPToolRegistry.h"

namespace WorldDataMCP
{
	namespace
	{
		TArray<FMCPToolModule>& MutableRegistry()
		{
			static TArray<FMCPToolModule> Registry;
			return Registry;
		}
	}

	void RegisterMCPToolModule(const FMCPToolModule& Module)
	{
		if (Module.GetDefinitions == nullptr || Module.Dispatch == nullptr)
		{
			return;
		}

		TArray<FMCPToolModule>& Registry = MutableRegistry();
		for (const FMCPToolModule& Existing : Registry)
		{
			if (Existing.GetDefinitions == Module.GetDefinitions && Existing.Dispatch == Module.Dispatch)
			{
				return;
			}
		}
		Registry.Add(Module);

		// Stable sort so modules sharing a Priority preserve registration order, matching
		// the deterministic ordering the legacy compile-time table relied on.
		Registry.StableSort([](const FMCPToolModule& A, const FMCPToolModule& B)
		{
			return A.Priority < B.Priority;
		});
	}

	const TArray<FMCPToolModule>& GetRegisteredMCPToolModules()
	{
		return MutableRegistry();
	}
}
