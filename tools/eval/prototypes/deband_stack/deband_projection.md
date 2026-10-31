<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Deband vs the master's own fine texture / grain (projection; see deband_stack.projection)

| domain | variant | master texture kept (proj) | added non-master fine energy |
|---|---|---|---|
| SDR gamma | oracle | 0.861 | 0.041 |
| SDR gamma | oracle+vpdb | 0.854 | 0.241 |
| SDR gamma | oracle+lpdb | 0.798 | 0.267 |
| HDR PQ (pre-TM) | oracle | 0.869 | 0.032 |
| HDR PQ (pre-TM) | oracle+vpdb | 0.868 | 0.067 |
| HDR PQ (pre-TM) | oracle+lpdb | 0.863 | 0.068 |
