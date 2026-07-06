# CitySample (Small_City) Houdini → PCG 提炼规范

> 把 Epic CitySample 的 Houdini 程序化城市生成工具集，提炼成项目 PCG 工具集可用的**规则目录 + 实例 schema + 算子映射表**。
> 这是知识/设计层产物：供 PromptPCGBridge / WorldDataMCP PcgKnowledge 检索，并作为 NativePCGRuleBridge 写原生规则的蓝本。

## 0. 来源与提炼方法

- **源目录**：`D:\CitySample_local_backup_20260618_210600\CitySample_HoudiniFiles\Small_City`
- **核心资产**：`houdini/otls/*.hda`（9 个 OTL）、`Small_City.hip` / `Small_City_Freeway.hip`、`python2.7libs/freeway_export`、导出产物 `EXPORT/*.bgeo` 与 `PBC/*.pbc`。
- **提炼方式**：
  - HDA 的 **DialogScript（参数接口）以明文存储** → 已抽出全部参数标签（规则旋钮）。节点网络 / VEX 为压缩段，本机无 Houdini 无法解包，故算法细节按参数语义 + CitySample 公开管线推断，标注 ⚠️ 处需在 Houdini 内复核。
  - `.pbc` 实测为 **Ogawa/Alembic 点云**（`AbcGeom_Points`），属性名通过二进制扫描确认。
  - 原始抽取串：`Plugins/UEBridgeMCP/Tools/reports/citysample_hda_strings/*.txt`（每个 HDA 一份，含全部参数标签，可追溯）。

---

## 1. 管线总览（4 阶段）

CitySample 的本质：**Houdini 规则生成城市 → 导出"每点=一个实例"的点云 `.pbc` + 烘焙几何 `.bgeo` → Unreal 读点云实例化**。这与 PCG「点 → 刷网格」完全同构，是整套东西能搬进 PCG 的根本原因。

```
                 ┌─────────────────────────────────────────────┐
   City_Layout → │ 1. BASE      road network(层级) → blocks      │
   (layout/zone) │              → zones/biomes → lots → ground   │
                 ├─────────────────────────────────────────────┤
   Building   →  │ 2. BUILDINGS lots → footprints → BDF 体量     │
   processors    │              → 楼层切片 → 风格/屋顶 → 墙面props │
                 ├─────────────────────────────────────────────┤
   City_Utils →  │ 3. FURNITURE packing 散布 + zone/sidewalk     │
   processors    │              街道家具 + 停车位 + 车辆 + 交通   │
                 ├─────────────────────────────────────────────┤
   tile_parser → │ 4. EXPORT    pointcloud_exporter → .pbc(实例) │
   pc_exporter   │              tile_parser → .bgeo(几何)         │
                 └─────────────────────────────────────────────┘
```

导出分类（来自 City_Processors EXPORT 分组 + `PBC/` 实际文件）：
`buildings · ground · street_furniture(sidewalk / biome_dependant) · cars_parked · parkings · decals · traffic · FX · audio` ＋ freeway 系列 `barriers · pillars · biomes · decals · deck_visible/collision · sand_light · traffic_data · trash`。

算子全表（从 HDA optype 抽出）：

| HDA | 算子 | 角色 |
|---|---|---|
| City_Layout | `city::layout` `city::zone` | 城市级布局 / 分区 |
| City_Processors | `city::{road,sidewalk,lot,ground,decal,street_furniture,traffic_data}_processor` `street_furniture_zone_dependant` `city::processor` | 各资产层处理器 |
| City_Utilities | `city::{tile_parser,packing_scatterer,parking_spaces_from_lots,road_block_maker,building_cubifier,building_profiler,car_alignment}` | 工具/散布/打包 |
| Building | `building::{building_generator,volume_slicer,volume_filter,volume_curate,bdf_to_bgeo,attribute_visualizer}` | 建筑体量与生成 |
| freeway_* | `freeway::{builder,constructor,connection,ramp,cross_section,pillars,barriers,traffic,pointcloud_exporter,...}` | 高速公路 |
| common_utilities | `utils::{curve_directions,inward_normal_on_polyline,orient_attribute,carve_by_distance,find_points_angles,...}` | 几何原语 |

---

## 2. 实例点契约（`.pbc` Alembic → PCG Point）★核心

`.pbc` = Alembic `AbcGeom_Points`，每点一个实例。**这是把 CitySample 接进 PCG 的 schema**，等价于一个带网格描述符的 `FPCGPoint`。

| Alembic 属性 | 类型 | 含义 | PCG 对应 |
|---|---|---|---|
| `P` | float3 | 位置 | `FPCGPoint.Transform` 平移 |
| `orient` | quat | 朝向（优先） | Transform 旋转 |
| `N` + `up` | float3×2 | 法线+up（无 orient 时用于构造朝向） | 由 N/up 算 rotation |
| `scale` | float3 | 三轴缩放 | Transform 缩放 |
| `pivot` | float3 | 实例枢轴偏移 | 烘进 Transform 或 BoundsMin/Max |
| `unreal_instance` | string | StaticMesh 资产路径，形如 `StaticMesh'/Game/.../SM_x.SM_x'` | StaticMeshSpawner 的 mesh / `MeshEntry` |
| `unreal_material` | string | 可选材质覆盖 | 材质 override attr |
| `.pointIds` | int | 点稳定 id | 可选稳定性/调试 |

**坐标系注意（⚠️ 复核点）**：Houdini Y-up、米、右手；UE Z-up、厘米、左手。导出时通常已做 Y↔Z 与 ×100 转换并把朝向写进 `orient`；落地 PCG 时要确认轴向/单位是否需再转换。

**落地建议**：在 PCG 工具集加一个 **Alembic `.pbc` → PCG Point 导入规则/节点**，读取上表属性产出标准 PCG 点集，再接 `StaticMeshSpawner`。这条打通后，**CitySample 既有点云可直接在 PCG 内重放**，无需 Houdini 重跑——优先级最高的互通基础。

---

## 3. 阶段提炼 + 算子→PCG 映射

每节给出：Houdini 规则旋钮（DialogScript 实抽）→ PCG 节点/原生规则 → 在本项目 `NativePCGRuleBridge` config 词汇里怎么表达。

### 3.1 BASE — 路网 + 地块布局

**Houdini 规则旋钮**（`city::layout` / `city::zone` / road/lot/ground processor）：
- 道路层级宽度：`Arterial / Collector / Local / Back-Alley / Sub-Alley Road Width`、`Freeway Road width`
- 街区：`Road Block Size`、`city grid size x/z`、`max_block_lenght_x/z`、`merging block percent` + `merging_block_seed`、`min_small_road`、`remove_small_local_road` + `removal_factor`、`use_noise_function_removal`、`road creation threshold`、`shallow angle roads`
- 截面要素：`Sidewalk Width/thickness`、`Divider_Width`、`Parking Lane Width`、`road thickness`、`Arterial/Collector road sidewalk enlargement`、`Inner Sidewalk`
- 分区：`zone_shape`（Hexa / Octa / Diamond / Plus / O-Cut）、`Zone_attributes`、`buffer_width`、`density`
- 地块：`lot_subdivision` / `lot_min` / `lot_max`、`setback`、`setback_floor_limit`、`New York lot min/max size`

**PCG 映射**：
- 路网骨架 = 样条网络。PCG 侧用 **Get Spline Data / Spline Sampler** 沿路采点；街区网格用 **Create Points Grid** + 按 block size 切分；删小路 = **Density Filter / Attribute 比较 + Self-Pruning**。
- 道路截面（车道/人行道/分隔带/路缘）= 沿样条按"距中心线偏移带"分配 mesh 段（项目里 `roads-are-spline-sampled` 记忆即此思路：spline 采样而非搬瓦片）。
- 地块 = 街区多边形按 `lot_min/max` 细分 → 每地块一个 attribute（zone/biome/lot_id）。
- 分区形状（Hexa/Octa/…）= 用 Voronoi/网格变体给地块打 biome 标签（PCG 内可用噪声 + 区域划分近似，⚠️ 精确形状需复核 HDA）。

**建议新增 config**（对齐现有 `FNativePCGRuleConfig` 风格）：
```
FNativePCGCityBlockModuleConfig {
  float ArterialWidth, CollectorWidth, LocalWidth, BackAlleyWidth, SubAlleyWidth;
  FVector2D BlockSize;          // road block size x/z
  float MergeBlockPercent; int MergeSeed;
  float SidewalkWidth, DividerWidth, ParkingLaneWidth;
  EZoneShape ZoneShape;         // Hexa/Octa/Diamond/Plus
  float RemoveSmallRoadFactor; bool bUseNoiseRemoval;
  FVector2D LotSizeRange;       // lot_min..lot_max
  float Setback;
}
```

### 3.2 FURNITURE — 散布 / 街道家具（最贴近现有框架，优先落地）

**Houdini 规则旋钮**（`packing_scatterer` / `street_furniture_processor(+zone_dependant)` / `parking_spaces_from_lots` / `car_alignment`）：
- packing 无重叠散布：`Island Padding`、`Island Rotation Step`、`filling_ratio`、`random and retry` / `retry`、`searching_type`、`Search Resolution`、`Distance Between Passages`
- 放置：`setback`、`height_offset`、`scale based`、`stepping`
- 停车：`parking_lane_width`、`number_of_lane`、`parking`
- 车辆：`car_alignment`（沿路朝向）、`cornering`

**PCG 映射**（直接对应现有 `FNativePCGSpawnLayerModuleConfig`）：
- 散布 = **Surface Sampler**（密度 = `BasePointsPerSquareMeter`，对应 `density`）→ **Self Pruning / Remove Duplicates** 实现 packing 无重叠（`Island Padding` = pruning 半径）→ **Transform Points** 随机缩放/旋转（`ScaleMin/Max`、`RotationZMin/Max`，旋转步进 = `Island Rotation Step`）→ **Static Mesh Spawner**。
- zone-dependant = 按地块 biome attribute **过滤/分流**（Density Filter / Attribute Partition）后挂不同 SpawnLayer 网格集。
- 街道家具网格集（已在源资产里）：路灯(Kit_StreetLamp_A/B)、垃圾桶(Kit_Trashcan_A)、消防栓、报刊亭、长椅、护栏、交通锥、树(Alder/Birch/Maple)…→ 各为一个 `FNativePCGSpawnLayerModuleConfig.Meshes`。

**这是最快出成果的一块**：现有 SpawnLayer + Surface Sampler 框架几乎可直接表达，只需补一个 **packing(Self-Pruning) 模块**：
```
FNativePCGPackingScatterModuleConfig {
  float IslandPadding;       // self-prune 半径
  float RotationStepDegrees; // Island Rotation Step
  float FillingRatio; int MaxRetries;
  bool bAlignToSurfaceNormal;
}
```

### 3.3 BUILDINGS — 建筑生成

**Houdini 规则旋钮**（`building_generator` / `volume_slicer/filter/curate` / `building_cubifier/profiler`）：
- 数据驱动：**BDF（Building Definition File）** 体量定义 → `bdf_to_bgeo` → generator；`Multi_BDF`、`Filter BDF Volume Tags`、`Tag Volumes`(fire escapes/brand overrides)
- 体量/楼层：`volume_slicer`（按层高切片）、`Internal Volume Type/Size`、`2 vs 3 layers building`、`extrude_height`、`Building_height_control`
- 风格：`Building_style`、`Cubified vs Empire State style`、`New York Style buildings`、`High Rise percent/seed`、`High rise height threshold`、`New York bldg height threshold`、`New York lot max/min size`
- 屋顶/细节：`Generate Roof`、`Roof Inset` / `Sub Roof Inset`、`Roof Offset Z`、`Place Props`(墙面) + `Prop Placement Threshold` + `Props Seed Offset`、`Place Ground Decals`、`setback_floor_limit`

**PCG 映射**：
- 地块多边形 → 体量挤出（PCG 5.4+ 可用 **Spline/Shape → Mesh** 或外部体量），按 `层高` 切片成楼层带 → 每段刷模块化建筑 kit（源资产 `Modular_Building_*` 一整套：1st/2nd/3rd 楼层、窗、门、阳台、屋顶 kit）。
- 风格 = 选择不同 kit 集 + 高度规则（high-rise %/threshold 决定塔楼）。
- 墙面 props / 屋顶设备（`Kit_roof_*`、AC、exhaust）= 在楼层/屋顶面上再跑一层 Surface-Sampler 散布。
- ⚠️ 建筑是最复杂一块，PCG 内做完整 BDF 切片不现实；务实路线是 **PCG 负责"地块→体量+楼层带+模块刷放"，复杂立面仍用模块化 kit 拼**。

```
FNativePCGBuildingMassingModuleConfig {
  EBuildingStyle Style;          // Cubified/EmpireState/NewYork
  float FloorHeight; int FloorCount;        // 或 height range
  float HighRisePercent; int HighRiseSeed; float HighRiseHeightThreshold;
  float Setback; int SetbackFloorLimit;
  bool bGenerateRoof; float RoofInset, RoofOffsetZ;
  float WallPropThreshold; int PropsSeedOffset;
}
```

### 3.4 几何原语（common_utilities → PCG）

写上面规则要用的曲线/属性工具，PCG 已有等价物或可作 Blueprint/原生节点补齐：

| utils 算子 | 用途 | PCG 等价 |
|---|---|---|
| `curve_directions` / `inward_normal_on_polyline` | 沿路/沿地块边算切向、内法线 | Spline Sampler 自带 tangent/normal；或自定义属性节点 |
| `orient_attribute` | 由 N/up 合成 orient 四元数 | Transform from attributes |
| `carve_by_distance` | 按距离切分样条 | Spline Sampler(by distance) |
| `find_points_angles` / `average_area` | 角点检测/面积 | 属性运算 |
| `packing_scatterer` | 无重叠打包散布 | Self Pruning / Remove Duplicates |

---

## 4. 落地 backlog（映射到 NativePCGRuleBridge 框架）

现框架：`INativePCGGenerationRule::BuildGraph(UPCGGraph*, FNativePCGRuleContext)` + 模块化 `FNativePCGRuleConfig`（Surface / Path / SpawnLayers[]）+ `UNativePCGRulePreset` DataAsset（参考 `ForestPathRule`）。建议按下序新增规则与 config：

| 优先级 | 新增规则 (INativePCGGenerationRule) | 新增 module config | 说明 |
|---|---|---|---|
| **P0** | `UPbcInstanceImportRule` | `FNativePCGInstanceSchema`(见 §2) | Alembic `.pbc` → PCG points → StaticMeshSpawner，打通互通，可立即重放既有城市 |
| **P0** | `UStreetFurnitureScatterRule` | `FNativePCGPackingScatterModuleConfig` + 复用 `FNativePCGSpawnLayerModuleConfig` | 最贴近现有框架；packing=Self-Pruning |
| P1 | `UCityBlockLayoutRule` | `FNativePCGCityBlockModuleConfig` | 路网层级 + 街区 + 地块 + biome 标签 |
| P2 | `UBuildingMassingRule` | `FNativePCGBuildingMassingModuleConfig` | 地块→体量→楼层带→模块化 kit 刷放 |
| P3 | freeway/parking/traffic 规则 | 各自 config | 高速、停车位、交通样条 |

每个规则配套 `UNativePCGRulePreset` 预设（如 `small_city_default`），让 PromptPCGBridge / MCP 可按名调用。

> 若后续要把本规范喂给 MCP（让 agent 直接调用生成），可把 §3 各规则作为 `patterns`、§1 管线作为 `workflows`、§2 schema 作为 `docs` 追加进 `Data/pcg_knowledge.json`（其 schema：nodes/patterns/workflows/docs）。本轮按"文档形态"未改动该 json。

---

## 5. 需在 Houdini 内复核的点（⚠️）

1. 节点网络/VEX 为压缩段，本机未解包；各算子**算法细节**（如 packing 的具体 search 策略、zone 形状几何、building 切片规则）按参数语义推断，落地前在 Houdini 打开 HDA 复核。
2. `.pbc` 朝向/单位是否已做 Houdini(Y-up,m) → UE(Z-up,cm) 转换（看 §2）。
3. `unreal_instance` 路径指向 `/Game/City/Small_City/...`，落地需确认这些 SM 资产在本项目存在或建立映射。
