# UnrealClaude Sequencer Bridge

An unofficial Unreal Engine 5 editor-plugin derivative that extends [Natfii's UnrealClaude](https://github.com/Natfii/UnrealClaude) with careful, MCP-driven Sequencer authoring.

It is designed for project-local use: the AI can inspect the active sequence and make narrow, explicit edits without guessing which actor, rig, control, or asset you intended.

## What this edition adds

- Inspect a Level Sequence: playback range, bindings, and existing tracks.
- Bind an explicitly named actor from the open editor world.
- Set transform keys for bound actors at an exact frame.
- Add an Anim Sequence clip to a specific skeletal binding.
- Add a Sound asset to the sequence master audio track.
- Inspect Control Rig tracks and list their exact named controls.
- Write a transform key to one selected Control Rig control at one selected frame.

The Control Rig operation requires the caller to inspect the sequence first and to send an exact rig index and control name. It preserves transform channels that are not supplied instead of silently resetting them.

## Safety model

- The bridge only operates while Unreal Editor is running and connected locally.
- Sequencer actions require explicit asset paths, binding IDs, actor names, rig indices, or control names.
- It does not launch renders, modify engine files, disable security, or make system-wide changes.
- Build output, local Node dependencies, logs, and secrets are intentionally excluded from version control.

## Installation

1. Copy or clone this folder to `<YourProject>/Plugins/UnrealClaude`.
2. Open the project in Unreal Engine and allow the editor to rebuild the plugin if prompted.
3. In `Resources/mcp-bridge`, install the Node dependencies with the package manager specified by `package-lock.json`.
4. Configure your MCP client to use the local bridge endpoint described in `Resources/mcp-bridge/README.md`.
5. Restart Unreal Editor after updating bridge source so the tool registry reloads.

This edition has been compile-validated with Unreal Engine 5.7 on Windows. Runtime edits should first be tested on a duplicate Level Sequence or a version-controlled project.

## Quick Sequencer workflow

1. Call `sequencer` with `operation: inspect` and a Level Sequence path.
2. For hands or other Control Rig work, call `inspect_control_rigs`.
3. Select the exact `control_rig_index` and `control_name` returned by inspection.
4. Call `set_control_rig_transform_key` with only the channels you want to change and an exact frame.
5. Save/test the sequence in Unreal before continuing to the next shot.

## Licensing and attribution

The Unreal plugin source is released under MIT; see [LICENSE](LICENSE). The bundled MCP bridge retains its own attribution-aware MIT license in [Resources/mcp-bridge/LICENSE](Resources/mcp-bridge/LICENSE). Upstream credit is retained in [NOTICE.md](NOTICE.md).

## Support

See [SUPPORT.md](SUPPORT.md). Add only a maintainer-controlled support URL before public release.
