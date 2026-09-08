# Configurable Interface Filtering and Compact NET View

This fixes https://github.com/aristocratos/btop/issues/214 and so our eventual PR shall say so in the description to
trigger closing that issue. The user will himself do any pushing to the git remote, but you the agent shall compose
conventional git commits along the way. This will require signing commits with the user's gpg key, which requires git
config local scope to set email w@qrk.us and name Kenneth Bingham. Ensure commits represent coherent, reversible, atomic
hunks of work. We'll probably end up with a single squashed commit in the end, but these will be useful for observing
our progress and managing change during testing.

## Summary

Add persistent interface filtering, exact startup-interface selection, and a compact multi-interface NET view. Shared NET code owns filtering, sorting, paging, and selection; platform collectors continue collecting every interface unchanged.

Use `iface_compact_view_active` for the runtime mode, distinct from the persistent `iface_view` startup preference.

## Configuration and Startup

- [x] Add serialized settings:

  - [x] `iface_include = ""`: a case-sensitive POSIX extended regular expression searched against the complete interface name; empty includes every collected, monitorable interface.
  - [x] `iface_exclude = ""`: a case-sensitive POSIX extended regular expression searched against the complete interface name; empty excludes no interfaces.
  - [x] `iface_view = "detail"`: `detail` or `compact`.
  - [x] `iface_sorting = "total"`: `total`, `speed`, or `alnum`.
  - [x] `iface_reversed = false`: reverse the natural order.

- [x] Remove the old substring and leading-`!` interface-filter behavior. Existing configurations containing the now-unknown `iface_filter` setting are ignored under normal btop compatibility behavior; do not migrate it automatically.
- [x] Update the generated/default `btop.conf` comments beside both directives to identify them explicitly as POSIX ERE patterns and document their empty-value behavior.

- [x] Sort criteria are:

  - [x] `total`: cumulative kernel download plus upload bytes, descending.
  - [x] `speed`: current download plus upload bytes per second, descending.
  - [x] `alnum`: natural interface-name order, ascending, so `eth2` precedes `eth10`.
  - [x] Equal totals or speeds use `alnum` ascending as the tie-breaker. Reversal reverses the complete order.

- [x] Preserve `net_iface` as the exact configured startup interface and add `--iface IFACE_NAME`, which overrides it. Resolve `--iface` first and fall back to `net_iface`; both use shared exact, case-sensitive validation and are not filter expressions.
- [x] Validate an explicit interface against the unfiltered collected, monitorable set. If it exists, force it into `confirmed_interfaces` and select it even when it fails `iface_include`, matches `iface_exclude`, or both. If it is absent, fail startup regardless of whether it came from `btop.conf` or `--iface`.
- [x] On fatal mismatch, print `Interface "NAME" not found` to stderr, log when possible, restore the terminal, and exit nonzero before entering the normal update loop.
- [x] Without an explicit selector, select the first eligible interface under the configured ordering. If no ordinary interface is eligible, run with the existing empty NET state.
- [x] Do not add asynchronous or time-limited selection. Centralize the existing cumulative RX+TX comparator without changing the discovery-order `Net::interfaces` vector.
- [x] Compile include and exclude independently at startup. Log an invalid directive and treat only that directive as empty, preserving the other valid directive.
- [x] Keep runtime-only state out of serialization: `iface_filtering`, `iface_compact_view_active`, `confirmed_interfaces`, `iface_index`, `iface_page`, both draft pattern texts, the active editor field, and filter-clear ownership.

For each collected interface name, use regex-search semantics rather than whole-string matching (users may add `^` and `$` themselves):

```text
included = iface_include.empty() || regex_search(name, iface_include)
excluded = !iface_exclude.empty() && regex_search(name, iface_exclude)
eligible = included && !excluded
```

Exclusion wins for ordinary interfaces that match both patterns. The complete behavior is:

| Include match | Exclude match | Explicit selector | Result |
|---|---|---|---|
| Yes | No | No | Included |
| Yes | Yes | No | Excluded |
| No | No | No | Excluded |
| No | Yes | No | Excluded |
| Any | Any | Yes, exact name exists | Force-included and selected |
| Any | Any | Yes, exact name absent | Fatal startup error |

## Filtering, Clearing, and Interaction

- [x] Implement interface include/exclude filtering, sorting, paging, and selection helpers once in shared NET code; do not duplicate them in platform collectors or change how they collect monitorable interfaces.
- [x] `I` opens one labeled interface-filter dialog containing include and exclude fields initialized from the currently confirmed pair.
- [x] During interface-filter editing:

  - [x] Tab and Shift-Tab switch the active field. Printable text, cursor movement, and Backspace edit the active field normally.
  - [x] `Enter` validates and confirms both fields atomically; typing does not mutate the confirmed pair or interface set.
  - [x] If either pattern is invalid, identify the invalid field, keep the dialog open, and preserve the last confirmed pair without partially applying either draft. Report both fields if both are invalid.
  - [x] `Esc` or an outside click cancels both drafts and preserves the prior confirmed pair.
  - [x] Ignore Delete and unrelated shortcuts while editing.

- [x] On successful interface-filter confirmation, rebuild and sort `confirmed_interfaces`, force and select a still-present explicit selector, otherwise preserve the selected name if still eligible or select the first eligible result, clamp page/index state, activate compact view, and make the interface filters the current Delete target.
- [x] Interactive filtering may remove the currently selected ordinary interface; this is not fatal. Select the first remaining match or enter the empty state. A present explicit selector remains force-included and selected.
- [x] Empty confirmed include and exclude patterns restore every collected, monitorable interface.
- [x] Introduce a runtime filter-clear target with `proc` and `iface` values:

  - [x] Successful confirmation of `I` makes `iface` the target.
  - [x] Successful confirmation of process `f` or `/` makes `proc` the target.
  - [x] Cancellation does not change the target.
  - [x] At startup, default to `proc` for backward compatibility.
  - [x] Treat the `iface` target as empty only when both `iface_include` and `iface_exclude` are empty.
  - [x] If the current target is empty and the other target is nonempty, Delete targets the nonempty one.
  - [x] If both targets are nonempty, Delete clears the most recently confirmed target.
  - [x] If both targets are empty, Delete does nothing.

- [x] Display the `del` clearing hint only beside the filter that physical Delete currently targets. Give mouse hints target-specific actions so clicking the PROC hint always clears `proc_filter` and clicking the NET hint always clears both interface patterns, independent of the last keyboard target.
- [x] Clearing the interface filters sets both `iface_include` and `iface_exclude` to empty atomically, rebuilds the confirmed set with all interfaces, and preserves selection when possible. Clearing `proc_filter` retains existing process-list behavior.
- [x] In compact view:

  - [x] `n`/`b` move through the confirmed order across pages.
  - [x] `Enter` opens the selected interface in the existing detailed view without changing persistent `iface_view`.
  - [x] `I` reopens filtering.
  - [x] `S` cycles `total → speed → alnum`.
  - [x] `R` reverses ordering, following `top`.
  - [x] Resorting preserves selection by name and reveals its new page.
  - [x] An empty set displays `No interfaces match include/exclude patterns — I to edit` and disables navigation and `Enter`.

- [x] Handle compact/editor input before PROC input. Existing process editor behavior and lowercase process actions remain unchanged except for recording successful process-filter confirmation as the Delete target.
- [x] On inventory changes, recompute the same include/exclude predicates, sort normally, preserve selection by name when possible, and clamp paging. Runtime disappearance of an initially valid explicit interface shows the planned empty/error NET state without terminating btop; if it reappears, force-include and select it again.

## Compact Rendering and Scaling

- [x] Preserve the NET outer box, selected interface name between `b`/`n`, selected IP address, and stationary right-hand statistics box. The statistics box tracks the selected compact tile.
- [x] Replace the detailed graph area with a responsive grid:

  - [x] Three rows per tile: selection badge and truncated name, download meter and current speed, upload meter and current speed.
  - [x] Use existing NET download/upload gradients, symbols, and units.
  - [x] Indicate selection with a high-contrast badge and bold interface name.
  - [x] Target 24 columns per tile; compute columns as `max(1, available_width / 24)` and rows as `max(1, available_height / 3)`.
  - [x] Truncate names first on narrow tiles, then omit numeric speeds before reducing meters below three columns.
  - [x] Show `page X/Y` and `total`, `speed`, or `alnum` in available header space. No direction indicator is required.

- [x] In the empty-filter-result state, retain the NET frame and controls, clear interface-specific name/IP/statistics, and show `No interfaces match include/exclude patterns — I to edit` in the graph area.
- [x] Use cached one-line graduated meters and already collected speeds, adding only O(visible interfaces) work.
- [x] With `net_auto = true`, use one visible-page ceiling:

  - [x] Sample the largest current download or upload speed among visible tiles once per normal refresh.
  - [x] Raise immediately to `max(10 KiB/s, sample × 1.3)` when reached.
  - [x] After five refreshes below one tenth, lower to `max(10 KiB/s, sample × 3.0)`.
  - [x] Reset scale-down state when the page changes or traffic rises.
  - [x] Display the ceiling in the header.

- [x] With `net_auto = false`, use and display independent configured download/upload ceilings.
- [x] Compact mode performs no additional system calls, kernel reads, collection, or background work.

## Tests and Delivery

- [x] Extend upstream GoogleTest coverage for:

  - [x] Empty patterns, include-only, exclude-only, both patterns, overlap with exclusion precedence, no matches, search semantics, case sensitivity, and clearing both interface patterns.
  - [x] Exact shared validation for `net_iface` and `--iface`, CLI precedence, selector override when a name fails include, matches exclude, or matches both, plus absent-name fatal errors, cleanup, stderr, and exit status.
  - [x] Invalid configured include and exclude patterns independently falling back to empty while preserving the valid counterpart and logging the invalid directive.
  - [x] Interface editing for both labeled fields, Tab/Shift-Tab switching, normal text editing, atomic Enter confirmation, field-specific invalid-pattern rejection, cancellation, and ignored keys; invalid or cancelled drafts must never partially apply.
  - [x] Delete ownership after interface and process confirmations, cancellation, empty-target fallback, both-filter behavior, keyboard clearing, and target-specific mouse hints.
  - [x] Startup preference versus runtime active state.
  - [x] Confirmation, zero matches, inventory disappearance and reappearance (including an explicit selector), paging, sorting, selection preservation, and detail entry.
  - [x] `total`, `speed`, and natural `alnum` ordering; numeric name segments; deterministic ties; reversal; sorting transitions; selection preservation.
  - [x] Grid geometry, narrow rendering, dynamic scaling, hysteresis, and fixed ceilings.
  - [x] Physical, tunnel, bridge, container, and `veth*` Mira-style names.
  - [x] Existing process filtering and lowercase process controls.
  - [x] Generated/default configuration comments identifying `iface_include` and `iface_exclude` as POSIX ERE patterns and documenting both empty-value behaviors.

- [x] Test structured model/layout output rather than complete ANSI snapshots. Run CMake/GoogleTest, native builds, static checks, and pseudo-terminal acceptance at minimum, narrow, and wide dimensions.
- [x] Before publishing, fork `aristocratos/btop` on GitHub, create the topic branch there, enable existing GitHub Actions, and ensure the expanded tests run. Prompt the user when the fork is needed.
- [x] Keep Mira’s actual filter and startup values only in the dotfiles-managed local configuration.
