# Capability Log — Unreal Editor MCP Bridge

A living record of **what the bridge/tools can and can't do**, so gaps get fixed
instead of forgotten. Whenever an agent (Claude or Codex) hits a wall — a tool
op that doesn't exist, silently no-ops, errors, or "the editor won't let me" —
**add an entry to 🔴 Open below** before moving on. When a gap is built + verified,
move it to 🟢 Resolved with how it was tested.

This doubles as the tool's public "known limitations" doc for the eventual GitHub release.

## How to log an entry

```
### <domain>.<operation> — <one-line title>
- Status: OPEN | PARTIAL | NEEDS-EDITOR | WONTFIX
- Category: bridge-node-factory | tool-gap | api-limit | bug | environment
- Date / Agent: YYYY-MM-DD / CLAUDE|CODEX
- Attempted: what you tried to do (exact op + params)
- Result: exact error, or "no such op", or "returned success but nothing changed"
- Fix idea: shortest path to make it work (or "needs editor / not fixable")
```

Categories:
- **bridge-node-factory** — the JS/C++ node factory can't create the node type you need.
- **tool-gap** — the tool exists but is missing this operation/field.
- **api-limit** — the underlying UE API makes it hard/impossible from a plugin.
- **bug** — it should work but errors/crashes/corrupts.
- **environment** — not a tool defect (build/toolchain/path), recorded so we don't re-chase it.

---

## 🔴 Open (fix these)

### blueprint.add_node — EventGraph node factory only supports ~11 primitive node types
- Status: OPEN — **HIGH priority (this is the other half of the gun gameplay port)**
- Category: bridge-node-factory
- Date / Agent: 2026-07-14 / CLAUDE (rediscovered), original from gun saga
- Attempted: build the Glock spawn/attach/fire gameplay in `BP_Player_Gun` EventGraph over MCP.
- Result: the node factory resolves ONLY `CallFunction, Branch, Event, VariableGet, VariableSet, Sequence, Add, Subtract, Multiply, Divide, PrintString`. It CANNOT create `SpawnActor`, `Cast`, Blueprint-interface **message** nodes, `AddComponent`, timelines, or delegate binds. `CallFunction` also does not resolve self/parent functions.
- Fix idea: extend the C++ node factory to spawn `UK2Node_SpawnActorFromClass`, `UK2Node_DynamicCast`, `UK2Node_Message` (interface call), `UK2Node_CallFunction` with self/parent scope resolution. Meaty but high value — unblocks porting whole gameplay systems (like the gun) hands-free.

### asset.set_asset_property — crashes the editor on array element paths
- Status: OPEN
- Category: bug
- Date / Agent: 2026-07-13 / CODEX
- Attempted: `set_asset_property` on `CompatibleSkeletons.0` (array element) of a Skeleton asset.
- Result: hard editor crash. Had to do it manually in the Retarget Manager.
- Fix idea: guard/validate array-index property paths; support `Array.N` append/set safely instead of crashing.

### widget — no property binding, no UMG animations
- Status: PARTIAL (designer editing works; these are missing)
- Category: tool-gap
- Date / Agent: 2026-08-12 / CLAUDE
- Attempted: n/a yet — known coverage gap of the new widget tool.
- Result: `widget` can add/remove widgets, set properties & canvas layout, compile — but cannot create **property bindings** (bind a widget prop to a variable/function) or author **UMG animations/timelines**. Non-canvas slot types only get generic reflection (may be unreliable).
- Fix idea: add `bind_property` (Get/Set binding) and an animation-track op later; add first-class handlers for VerticalBox/Overlay/Grid slots.

### material — no material functions, node deletion, or asset-level settings
- Status: PARTIAL (graph node authoring works; these are missing)
- Category: tool-gap
- Date / Agent: 2026-08-12 / CLAUDE
- Result: `material` graph ops can create/add/connect/set nodes, but can't add **Material Function Call** nodes, delete a node, set **blend mode / shading model / material domain / two-sided**, or read existing graph connectivity.
- Fix idea: add `delete_node`, `set_material_settings`, `add_function_call`, and connectivity to `list_nodes`.

### anim.set_anim_node_property — only 4 node types special-cased
- Status: PARTIAL (gun-critical cases work; broad coverage thin)
- Category: tool-gap
- Date / Agent: 2026-08-12 / CLAUDE
- Result: branch-filter bone / cache name / mesh-space flag are handled; a generic reflection fallback exists but is untested for most node types. Changing a Layered Blend's **layer count** is minimal; blend profiles unsupported.
- Fix idea: broaden after live testing reveals which node properties are actually needed.

---

## 🟢 Resolved (built + verified)

### anim — top-level AnimGraph pose-node authoring
- Resolved: 2026-08-12 / CLAUDE (commit 2696700). Compiler-verified clean (`BuildPlugin -WarningsAsErrors`). **Live test still pending.**
- Was: the bridge could edit state machines + event graphs but NOT the AnimGraph pose graph — no way to add Use/Save Cached Pose, Layered Blend Per Bone, or wire pose pins. The #1 wall of the gun saga.
- Now: `list_anim_graph_nodes`, `add_anim_graph_node`, `set_anim_node_property`, `connect_anim_pose`.

### widget — UMG designer editing
- Resolved: 2026-08-12 / CLAUDE (commit 87971ba). Compiler-verified. **Live test pending.**
- Was: no way to edit the widget designer tree (only the WBP event graph, via blueprint_modify).
- Now: `widget` tool — inspect / create_widget_blueprint / add_widget / remove_widget / set_widget_property / set_slot_property / compile.

### material — graph/node editing
- Resolved: 2026-08-12 / CLAUDE (commit 87971ba). Compiler-verified. **Live test pending.**
- Was: only material *instances* + parameters were editable.
- Now: `material` graph ops — create_material / list_nodes / add_node / set_node_value / connect_nodes / connect_property / recompile_material.

---

## ⚙️ Environment notes (not tool defects)

- **Full-project CLI editor build fails** on the `UE-FSR` marketplace plugin (`ActionGraphInvalid`) — pre-existing, unrelated to this plugin. Compile the plugin via **in-editor Live Coding**, or `RunUAT BuildPlugin` with a **short `-Package` path** (long paths trip UBT `CheckPathLengths` / Windows MAX_PATH). Engine: `E:\Unreal Engine\UE_5.7`.
