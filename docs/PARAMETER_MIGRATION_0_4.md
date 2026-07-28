# UE_AI_integration 0.4 Parameter Migration

Version 0.4.0 standardizes capability input parameters on lower camelCase.
The old snake_case names are rejected by manifest validation and are not
accepted as aliases.

| Before | 0.4.0 |
|---|---|
| `actor_name` | `actorName` |
| `align_to_normal` | `alignToNormal` |
| `animation_name` | `animationName` |
| `area_class` | `areaClass` |
| `auto_size` | `autoSize` |
| `base_size` | `baseSize` |
| `block_size` | `blockSize` |
| `blueprint_name` | `blueprintName` |
| `bounds_max` | `boundsMax` |
| `bounds_min` | `boundsMin` |
| `cell_size` | `cellSize` |
| `child_class` | `childClass` |
| `child_name` | `childName` |
| `commandlet_name` | `commandletName` |
| `content_name` | `contentName` |
| `csv_path` | `csvPath` |
| `decorator_class` | `decoratorClass` |
| `design_size_mode` | `designSizeMode` |
| `design_time_size` | `designTimeSize` |
| `display_rate` | `displayRate` |
| `emitter_template` | `emitterTemplate` |
| `end_time` | `endTime` |
| `event_name` | `eventName` |
| `function_name` | `functionName` |
| `host_name` | `hostName` |
| `key_name` | `keyName` |
| `key_type` | `keyType` |
| `level_path` | `levelPath` |
| `max_scale` | `maxScale` |
| `min_scale` | `minScale` |
| `new_index` | `newIndex` |
| `new_name` | `newName` |
| `new_parent_name` | `newParentName` |
| `node_index` | `nodeIndex` |
| `only_if_dirty` | `onlyIfDirty` |
| `output_dir` | `outputDir` |
| `output_path` | `outputPath` |
| `package_path` | `packagePath` |
| `param_name` | `paramName` |
| `param_type` | `paramType` |
| `parent_class` | `parentClass` |
| `parent_index` | `parentIndex` |
| `parent_name` | `parentName` |
| `preview_size` | `previewSize` |
| `row_name` | `rowName` |
| `row_struct` | `rowStruct` |
| `service_class` | `serviceClass` |
| `slot_name` | `slotName` |
| `start_time` | `startTime` |
| `static_mesh` | `staticMesh` |
| `step_size` | `stepSize` |
| `task_class` | `taskClass` |
| `value_type` | `valueType` |
| `wall_height` | `wallHeight` |
| `widget_bp` | `widgetBp` |
| `widget_name` | `widgetName` |
| `z_order` | `zOrder` |

Capability IDs are unchanged. For example,
`content.static_mesh.inspect` remains a dotted operation ID while the
`scene.actor.spawn` input field is now `staticMesh`.
