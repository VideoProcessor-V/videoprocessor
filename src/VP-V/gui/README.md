# VP-V GUI side

VP's parts of the executable's GUI, out of `VideoProcessorDlg.cpp` and `StatsOverlayWindow.cpp`
(Bill's files), which keep one call per site (WP-11, `docs/design/wp11-callsites.md`).

| Header | One job |
|---|---|
| `VpStatusAdapter.h` | The one place the dialog gets VP's status lines (a thin client of the one status reader, `modules/control/diagnostics/StatusExtension`, W1-C14): one poll of the renderer's VP module (`vpv.modules.v1` Status through `VpvCalls::GetModuleStatus`, module `vp`), the Ctrl+I extension rows, the crash-state keys, the Ctrl+Alt+D bundle lines, the content/room/key-override lines, and the VP shortcut dispatcher. MFC side, header-only |
| `ShortcutObserverStatus.h` / `.cpp` | The report key's default global action in VP-V code (owner 2026-11-12 Q4, 2026-11-15: Bill's file keeps its text). While `[shortcuts] global_actions` is absent (VP-V pathways only), `global_hotkeys` and `foreground_only` are off and the report key has a chord, VP-V's own low-level keyboard hook (its own thread, the `.cpp`) posts the report command to the dialog when that chord is pressed in another application; the key is never taken, and with VP or VP's Config in front it is theirs (ConfigurationLiveApply's shared rules, AltGr excluded). A `DialogExtension` on the existing `DialogExtension::OnTimer()` tick (no payload): it reads its facts from the configuration file with the shared readers every tick, so an explicit value or a new chord applies within a second; the pathway in force is read once (a `[vpv]` change takes effect at the next start). While the hook is wanted but not running, the "global shortcuts" Fallbacks entry (rule 5): `VpStatusAdapter.h` `Poll` adds it to the Fallbacks line, so the Config status and the one-time notice show it, and Ctrl+I, the crash state and the Ctrl+Alt+D bundle while a renderer runs. The `.cpp` registers it statically (GUI executable only, `build/VideoProcessor-GUI.items.props`, GUI precompiled header) |
| `VpShortcuts.h` | VP's command ids (moved out of `Resource.h`, same values), their `[shortcuts]` keys and their place in the binding order (`Merge`). Pure |

The wording and order live in `core/StatusText.h` (pure, CPU-tested). Tests:
`tests/StatusTextTests.cpp` (pure) and `tests/VpStatusAdapterTests.cpp` (old dialog code vs the
adapter, byte for byte, for representative states); `tests/ShortcutObserverStatusTests.cpp` (the
default report key: the facts read from a configuration file, the plan, the hook's key rules, the
restarts on a plan change, and the "global shortcuts" entry: wanted + failed gives one entry and one
notice, none in the Original pathway or while the hook runs).

Adding a VP status line: give the renderer's module a new key in `core/StatusText.h` (no plugin
API change), read it in `VpStatusAdapter.h`, add its row or crash-state key there, and add a
case to the tests. Adding a VP shortcut: a `ShortcutDefaults.h` entry, an id in the VP block of
`VpShortcuts.h`, a `kDefinitions` entry naming the key it follows, and a case in
`VpvGui::DispatchCommand` (renderer-side actions go through `InvokeModuleCommand`).
