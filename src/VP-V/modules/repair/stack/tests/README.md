# Tests for `repair/stack` (draft)

`RepairStackOrderTests.cpp` (VideoProcessor-Test, CPU only): the declared order TD -> deband -> contrast recovery, today's behaviour while
temporal detail is absent (the layers that run keep their order), the plan and the visible fallback status for every temporal-detail state,
the agreement with the catalog's pathway order, and the debander's plan.
