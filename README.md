# Unreal Editor MCP Bridge

An unofficial Unreal Engine 5 editor-plugin derivative that extends [Natfii's UnrealClaude](https://github.com/Natfii/UnrealClaude) with careful, MCP-driven Sequencer authoring and environment building.

It works with any MCP-compatible AI client — including Codex, Claude, and future clients — rather than being tied to one assistant.

It is designed for project-local use: the AI can inspect the active sequence and make narrow, explicit edits without guessing which actor, rig, control, or asset you intended.

## What this edition adds

- Inspect a Level Sequence: playback range, bindings, and existing tracks.
- Bind an explicitly named actor from the open editor world.
- Set transform keys for bound actors at an exact frame.
- Add an Anim Sequence clip to a specific skeletal binding.
- Add a Sound asset to the sequence master audio track.
- Inspect Control Rig tracks and list their exact named controls.
- Write a transform key to one selected Control Rig control at one selected frame.
- Inspect a Static Mesh before using it as a modular kit piece.
- Place an explicitly selected Static Mesh as a real `StaticMeshActor`.
- Build a simple modular room shell from a selected wall piece and explicit dimensions.
- Scatter deterministic foliage through an existing Foliage Type without changing its materials, collision, shadows, or culling settings.
- Inspect landscape actors, their bounds, transforms, and assigned materials.
- Audit an active level for instance counts, estimated mesh triangles, shadow casters, movable lights, Nanite use, material references, and repeated high-cost meshes.
- Audit a material's render mode and shader-risk patterns, including two-sided rendering, translucency, texture samples, scene textures, custom nodes, WPO-related world-position nodes, and runtime virtual textures.
- Start/stop deliberate `stat unit`, `stat gpu`, `stat rhi`, and Unreal Insights traces while the user profiles in PIE or a packaged build.

The Control Rig operation requires the caller to inspect the sequence first and to send an exact rig index and control name. It preserves transform channels that are not supplied instead of silently resetting them.

## Safety model

- The bridge only operates while Unreal Editor is running and connected locally.
- Sequencer actions require explicit asset paths, binding IDs, actor names, rig indices, or control names.
- It does not launch renders, modify engine files, disable security, or make system-wide changes.
- It does not sculpt or paint a Landscape automatically. Those edits need an explicit target landscape, edit layer, brush bounds, strength, and falloff before they are safe to automate.
- It does not infer a whole level's art direction or an arbitrary modular kit's pivot/orientation. Inspect one module, test one placement, then create the shell or placement batch.
- Build output, local Node dependencies, logs, and secrets are intentionally excluded from version control.

## Installation

1. Copy or clone this folder to `<YourProject>/Plugins/UnrealClaude`.
2. Open the project in Unreal Engine and allow the editor to rebuild the plugin if prompted.
3. In `Resources/mcp-bridge`, install the Node dependencies with the package manager specified by `package-lock.json`.
4. Configure your MCP client to use the local bridge endpoint described in `Resources/mcp-bridge/README.md`.
5. Restart Unreal Editor after updating bridge source so the tool registry reloads.

This edition has been compile-validated with Unreal Engine 5.7 on Windows. Runtime edits should first be tested on a duplicate Level Sequence or a version-controlled project.

## Efficient shared AI context

The bridge stays loaded for as long as Unreal is open. It uses the **compact** tool profile by default, exposing a small direct tool set plus one routed editor tool instead of loading every operation schema into every agent conversation. Tool definitions are cached for five minutes by default, and ordinary text responses are capped at 12,000 characters so broad listings do not consume a whole conversation. Set `UNREAL_MCP_TOOL_PROFILE=balanced`, `MCP_TOOL_CACHE_TTL_MS`, or `MCP_MAX_RESPONSE_CHARS=0` only when a client genuinely needs the expanded/unbounded behaviour.

Use `unreal_project_memory` to read or update `Docs/AI_HANDOFF.md`. It is a short, shared handoff for Codex, Claude, and other MCP clients: a current milestone summary plus recent bridge activity. Older activity is archived automatically, so it does not become a growing transcript. A client may call `register_agent` with its known model name; the Unreal panel then shows that client and model. The bridge deliberately does **not** invent a quota meter—remaining usage appears only when the client explicitly provides it.

## Quick Sequencer workflow

1. Call `sequencer` with `operation: inspect` and a Level Sequence path.
2. For hands or other Control Rig work, call `inspect_control_rigs`.
3. Select the exact `control_rig_index` and `control_name` returned by inspection.
4. Call `set_control_rig_transform_key` with only the channels you want to change and an exact frame.
5. Save/test the sequence in Unreal before continuing to the next shot.

## Quick world-building workflow

1. Search the project for the actual wall, prop, or foliage assets you want to use.
2. Call `world_builder` with `operation: inspect_static_mesh` to read the module's local dimensions and Nanite state.
3. Place one test piece with `place_static_mesh`; verify its forward axis and pivot in the level.
4. Use `build_room_shell` only after that test succeeds. It treats local X as the module length and leaves doors, windows, corners, roofs, and dressing as intentional choices.
5. Use `scatter_foliage` with an existing `FoliageType` and a seed. The same seed reproduces the exact scatter if you need to revise it.
6. Use `inspect_landscapes` to select the intended terrain before any future landscape tool is added.

## Performance and material workflow (UE 5.7)

1. Call `performance` with `operation: "scene_audit"` while the intended level is open. Start with the returned top meshes, shadow-caster count, movable lights, and instancing data; do not mass-disable shadows without checking the gameplay camera.
2. Call `performance` with `operation: "material_audit"` and an explicit material path (for example `/Game/Materials/M_Forest.M_Forest`). Treat its results as a shortlist for the Material Editor's **Stats** and **Shader Complexity** views.
3. Start PIE and call `performance` with `operation: "pie_capture"`, `capture_action: "start"`. Play normally through the expensive section, then call the same tool with `capture_action: "stop"`. The bridge records frame, game-thread, render-thread, and GPU samples continuously and saves a JSON report under `Saved/MCPPerformanceCaptures`.
4. Use `runtime_profile_command` with `stat_unit` and then `stat_gpu` when the capture points to a bottleneck. Those timings—not editor triangle counts—decide what needs optimizing.
5. For a deeper capture, call `start_trace`, play through the expensive area, then call `stop_trace` and inspect the trace in Unreal Insights.

## Narrative / subtitle trigger workflow

1. Call `narrative_trigger` with `operation: "list"` to read every placed `VoiceTrigger`, `RadioVoiceTrigger`, or `NarrativeTrigger` instance, including its assigned audio assets and exposed subtitle fields.
2. Call `subtitle_audit` before a subtitle pass. It reports triggers missing English text, Romanian text, a sound, or a per-instance subtitle enable flag.
3. Call `update_subtitles` with one exact `actor_name` plus `english`, `romanian`, `speaker`, `show_subtitle`, `show_speaker`, and/or `duration`. Only that placed actor is changed; Blueprint defaults and sibling trigger instances are untouched.
4. Run `read` on a trigger after the update to verify the actual Details-panel values before testing in PIE.

The audit is intentionally diagnostic. It will not alter scalability settings, shaders, Nanite, shadows, materials, or foliage automatically. That keeps optimization choices reviewable and prevents an AI from "fixing" frame time by silently damaging the game's look.

## Licensing and attribution

The Unreal plugin source is released under MIT; see [LICENSE](LICENSE). The bundled MCP bridge retains its own attribution-aware MIT license in [Resources/mcp-bridge/LICENSE](Resources/mcp-bridge/LICENSE). Upstream credit is retained in [NOTICE.md](NOTICE.md).

## Support

See [SUPPORT.md](SUPPORT.md). Add only a maintainer-controlled support URL before public release.
