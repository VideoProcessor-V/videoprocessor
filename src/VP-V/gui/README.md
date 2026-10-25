# VP-V GUI side

VP's parts of the executable's GUI, out of `VideoProcessorDlg.cpp` and `StatsOverlayWindow.cpp`
(Bill's files), which keep one call per site (WP-11, `docs/design/wp11-callsites.md`).

| Header | One job |
|---|---|
| `VpStatusAdapter.h` | The one place the dialog gets VP's status lines: one poll of the renderer's VP module (`IVideoRenderer::GetModuleStatus`, module `vp`), the Ctrl+I extension rows, the crash-state keys, the Ctrl+Alt+D bundle lines, the content/room/key-override lines, and the VP shortcut dispatcher. MFC side, header-only |
| `VpShortcuts.h` | VP's command ids (moved out of `Resource.h`, same values), their `[shortcuts]` keys and their place in the binding order (`Merge`). Pure |

The wording and order live in `core/StatusText.h` (pure, CPU-tested). Tests:
`tests/StatusTextTests.cpp` (pure) and `tests/VpStatusAdapterTests.cpp` (old dialog code vs the
adapter, byte for byte, for representative states).

Adding a VP status line: give the renderer's module a new key in `core/StatusText.h` (no plugin
API change), read it in `VpStatusAdapter.h`, add its row or crash-state key there, and add a
case to the tests. Adding a VP shortcut: a `ShortcutDefaults.h` entry, an id in the VP block of
`VpShortcuts.h`, a `kDefinitions` entry naming the key it follows, and a case in
`VpvGui::DispatchCommand` (renderer-side actions go through `InvokeModuleCommand`).
