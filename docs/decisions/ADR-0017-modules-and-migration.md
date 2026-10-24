# ADR-0017: Module folders, manifests and the migration directive

- Status: Accepted
- Date: 2026-10-21 (owner decision; recorded 2026-10-24)
- Slots: all slots

## Context

Modularity is the central tenet; future agents and successors must pick up any module cold; modules should be liftable into other programs.

## Decision

Every unit/slot has a folder src/modules/<unit>/<slot>/ from day one with README (module doc), MANIFEST.json (required/optional files, hardware, tools, config keys, upstream modules, fallback when missing), tests/ and a live file list pointing to where the code lives now. A master component list is generated from the manifests and drift-checked. No duplicate source: code moves only when touched until M1 (~11-06), then a dedicated migration phase finishing by M2; one module per change via git mv, includes/projects + live list + master list in the same commit, build + module tests + chain-gate subset passing. Module principles: small documented API, no hidden globals, injected dependencies, own config schema, standalone tests; why-comments, small functions, no magic numbers, one fallback pattern.

## Consequences / open

Definition of done for a slot change: update its module README in the same commit.
