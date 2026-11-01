# Skeleton hub (M2 P0)

`ExtensionHub` is the single `RendererExtension::IRendererExtension` that VP-V registers with the generic
hook layer (`src/VideoProcessor-Lib/vprenderer/RendererExtension.h`). It fans every callback out to
`ISkeletonModule`s in a fixed order and keeps per-host state keyed by the renderer identity.

- P0: no module is added, so the hub is inert (registering it changes nothing).
- Registration: `HubBootstrap.cpp`, kept by `/INCLUDE:VpvHubBootstrapAnchor` in
  `src/VP-V/build/VideoProcessor-VPRenderer*.items.props`. No line in the host files.
- Modules are added at start-up only (`AddModule`); the list is read-only afterwards and dispatch is lock-free.
- Not a module folder: no `MANIFEST.json` (core/ has none); the module checker does not scan it.
- Later packages add modules (resync/seal first, vector engine, entrance, motion, presentation, capture, camera/LED).
