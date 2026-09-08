# Agent Notes

## Handoff: Configurable interface filtering and compact NET view

### Objective

Finish the implementation described in `PLAN.md`: persistent POSIX ERE network
interface filtering, exact startup selection with recovery, configurable
sorting, and a compact paged NET view. Keep platform collectors responsible
for collecting every monitorable interface and centralize policy and runtime
state in shared NET code.

### Current state

All work is uncommitted in this checkout. The current changes compile with GCC
14 and the six registered GoogleTests pass, but the feature is incomplete and
should not be committed as-is.

Implemented so far:

- Added `iface_include`, `iface_exclude`, `iface_view`, `iface_sorting`, and
  `iface_reversed` config defaults, generated-config comments, validation, and
  NET Options entries.
- Added repeatable `--iface NAME` parsing (last value wins), help text, CLI
  precedence, initial unfiltered validation even with NET hidden, and reload
  reapplication.
- Added shared regex compilation/normalization, eligibility, natural sorting,
  reversal, selection preservation, and explicit-selector recovery in
  `src/btop_shared.cpp`.
- Wired every platform collector through the shared rebuild step.
- Added an initial compact renderer and `n`/`b`, Enter, `S`, and `R` controls,
  plus empty and unavailable messages and initial page-scale hysteresis.
- Added model tests in `tests/net_interfaces.cpp`.
- Added a Podman-first reproducible build environment in `Containerfile`,
  `.dockerignore`, `scripts/container-build`, `scripts/container-entrypoint`,
  and `CONTAINER_BUILD.md`. It copies the source snapshot into the image to
  avoid host ownership/filemode changes. Docker remains selectable with
  `CONTAINER_ENGINE=docker`.

### Proven verification

Run:

```sh
./scripts/container-build -DBTOP_GPU=OFF
```

This was verified rootlessly with Podman using GCC 14.3.0. CMake configured,
Lowdown generated the manpage, the executable and tests built, and all six
tests passed. `git diff --check` also passed. Upstream Linux CMake CI is the
toolchain/process reference: GCC 14, CMake, Ninja, Debug, then CTest.

### Immediate work

1. Review the partial implementation against every requirement in `PLAN.md`.
2. Replace the `#if 0` superseded selection blocks in all five platform
   collectors with clean removal; confirm collectors retain an unmodified raw
   inventory before shared filtering.
3. Implement the two-field `I` dialog: drafts from confirmed values,
   Tab/Shift-Tab and click field selection, normal editing, atomic validation,
   complete invalid-field reporting, Enter commit, and Escape/outside-click
   cancellation.
4. Implement Delete ownership between the process filter and the atomic
   interface-filter pair, including fallback and target-specific mouse hints.
5. Extract explicit testable helpers for selection/recovery, paging/layout,
   and scale hysteresis instead of leaving those policies embedded in drawing
   code.
6. Complete compact geometry, page handling, selected tile/IP/right-side
   statistics, fixed-mode independent ceilings, narrow-layout degradation,
   and O(visible interfaces) processing.
7. Add the remaining CLI/config/editor/reload/inventory-churn/layout/PTY tests
   from `PLAN.md`, then run the container build with default GPU settings and
   with `-DBTOP_GPU=OFF`.

### Cautions

- `PLAN.md` is the authoritative specification and is currently untracked.
- Do not treat `net_iface` or `--iface` as a permanent pin: after initial or
  recovery selection, navigation must remain possible.
- Selection must be exact and case-sensitive against the unfiltered inventory;
  filters use case-sensitive POSIX ERE search semantics, and exclusion wins
  except for the explicit selector.
- Total ordering must use raw cumulative kernel RX+TX counters; the displayed
  `z` offset must not influence it.
- Invalid include/exclude regexes normalize independently. Dialog confirmation
  is atomic, whereas config-load normalization is independent.
- Do not add per-refresh system reads. Compact work must remain bounded by the
  visible page.
- Preserve existing process-filter behavior and unrelated dirty work. Do not
  push. Before publication, ask the user to create/authorize the upstream fork
  and Actions setup.
- The upstream contribution guide requires AI-generated submissions to be
  explicitly marked as AI generated.

### Acceptance

The work is complete only when all behavior and test bullets in `PLAN.md` are
implemented, the supported local platform build passes in the container, the
available cross-platform variants compile, static checks and PTY acceptance
pass at minimum/narrow/wide sizes, and the changes are split into coherent,
revert-safe signed commits using repository-local identity
`Kenneth Bingham <w@qrk.us>` without pushing.

### Suggested skills

- `tdd` for completing the editor, selection, paging, layout, and hysteresis
  behavior test-first.
- `diagnose` if PTY rendering, reload, or inventory churn exposes regressions.
- `code-review` before forming the final commits, comparing both repository
  standards and `PLAN.md` compliance.
- `commit-ours` only after the implementation and verification are complete;
  note that its normal workflow may push/open a PR, which conflicts with this
  task's explicit no-push requirement unless the user changes that direction.
