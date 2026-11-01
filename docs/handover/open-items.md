# Open items

## Camera K1+K2+K4 (module side)
- Subtask 13, the offline benchmark (motion_bench on D:\testclips, EPE no-camera vs camera vs RAFT, narrow/escalated/skipped shares), is
  NOT run: limp L2 (coordinator, 2026-11-01). Run it after the usage reset, after the verification build; tools are in
  tools/eval/motion_bench/ (run_motion_bench.ps1, score_flow.py, offline_adapter.py).
- Wiring still to do: H1 adapter (CameraExtension), items.props (H7), K1-9 LaneRunner registration, p7g in SkeletonProducts()/ProductRows().
