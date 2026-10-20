# Smart region selection

### To be Reviewed By

* Flameshot maintainers
* macOS and Windows maintainers and users
* Contributors interested in a Linux backend

### Authors

* Gábor Fekete and contributors

### Status: Draft

### Superseded by

N/A

### Related

* Issue #5 (canonical window-selection request)
* Issue #4910 (ShareX-style macOS feasibility patch)
* Issue #4239 (window and panel selection)
* Issue #1814 (rectangle recognition)

## Problem

Flameshot's GUI capture mode starts with no selected area. Capturing a whole
window, dialog, toolbar, or other rectangular interface element therefore
requires carefully drawing its bounds by hand. This is slower and less precise
than region capture in tools such as ShareX, which detects the window or control
under the pointer.

The existing requests cover selecting a whole window, but users also benefit
from selecting a meaningful child element such as a panel, table, or dialog.
Flameshot must retain its current freehand rectangle workflow and its ability to
annotate the chosen region before copying or saving it.

## Anti-Goals

* This RFC does not add automatic capture or bypass the existing annotation UI.
* It does not require every platform backend to ship at the same time.
* It does not use computer vision to infer borders from screenshot pixels.
* It does not make inaccessible or hidden UI elements visible.
* It does not change Wayland security or compositor protocols.

## Solution

Add an optional smart region detector to GUI capture. Before the capture overlay
appears, the detector records the visible top-level window stack. While no area
is selected, moving the pointer over a detected region previews that region:

* a click commits the preview as the normal editable Flameshot selection;
* a drag keeps the existing custom rectangle behavior;
* scrolling up walks from the deepest detected element toward its parent and
  ultimately the containing window;
* scrolling down walks back toward the previously detected child;
* after an area is committed, the mouse wheel retains its current tool-size
  behavior.

The preview dims everything outside the candidate and draws a colored outline.
It deliberately avoids an inline role/index label because that label can cover
the content the user is trying to inspect. The selection can still be resized
and annotated using existing Flameshot tools.

The macOS backend uses native window and Accessibility APIs:

1. `CGWindowListCopyWindowInfo` records normal, on-screen windows in front-to-
   back order before Flameshot's overlay exists.
2. The window under the global pointer identifies the owning process.
3. `AXUIElementCopyElementAtPosition` is called with that application's
   accessibility object, so hit-testing remains restricted to the underlying
   application even though the Flameshot overlay is now topmost.
4. `kAXParentAttribute`, `kAXPositionAttribute`, and `kAXSizeAttribute` produce
   the selectable leaf-to-window chain.
5. Duplicate, tiny, invalid, and off-window rectangles are removed. The Window
   Server geometry remains a fallback when Accessibility access is unavailable.

The Windows backend follows the same contract:

1. `EnumWindows` records visible top-level windows before the overlay appears.
2. `DwmGetWindowAttribute` supplies extended frame bounds and filters cloaked
   windows, with `GetWindowRect` as a fallback.
3. Microsoft UI Automation starts from the recorded window handle, hit-tests
   its control-view descendants, and walks back through their parents.
4. UI Automation calls run on a dedicated MTA worker with connection and
   transaction timeouts, a request budget, and a bounded GUI wait. An unavailable
   provider or expired request falls back to the containing window.
5. Native screen coordinates are mapped to Qt logical coordinates per monitor,
   including mixed-scale displays.

The detector is behind a platform-neutral interface. Every backend returns the
same leaf-to-window candidate chain in global logical coordinates; the capture
widget owns preview, navigation, and selection behavior. Supported backends fall
back to window geometry when Accessibility or UI Automation is unavailable.
Unsupported platforms use a no-op detector and retain manual selection without
installing smart-region event handling.

The current fork implementation and proposed platform rollout are:

* macOS, implemented: Window Server geometry plus Accessibility hit-testing,
  as described above;
* Windows, implemented: top-level window enumeration plus UI Automation
  hit-testing and parent traversal;
* X11, planned: EWMH stacking and window geometry, with AT-SPI providing an
  optional child-element hierarchy. No Linux backend is implemented yet;
* Wayland, unsupported: a future compositor-specific backend requires APIs that
  provide the same contract. The current screenshot portals do not provide
  arbitrary foreign window and control geometry, so the no-op detector retains
  the manual workflow.

This fork implements the shared contract on macOS and Windows. This draft RFC
proposes that contract and staged native backends for upstream review; it does
not imply upstream acceptance or Linux support.

### Configuration and permissions

On macOS and Windows, a General setting named **Enable smart region selection**
controls the feature and is disabled by default. Enabling it is an explicit
choice because preselection clicks and scrolling gain new behavior. On macOS,
window snapping works without Accessibility permission, while selecting
controls inside another application requires the standard macOS Accessibility
permission. General settings provides an explicit **Grant Accessibility
Access...** button on macOS only. Windows UI Automation does not require that
permission UI.

## Performance Impact

Window enumeration occurs once per capture. Pointer hit-testing is throttled to
at most once every 35 milliseconds. Accessibility messaging and UI Automation
connection/transaction calls use 80-millisecond limits within a 200-millisecond
request budget. Windows waits up to 250 milliseconds for a worker result on the
GUI thread; macOS performs its budgeted lookup on the GUI thread. These are
best-effort limits, not process isolation or a guarantee against arbitrary
provider stalls or COM teardown delays. Painting uses only the current candidate
rectangle.

The candidate hierarchy is recalculated when the pointer moves. Scrolling only
changes an index in the already collected chain.

## Backwards Compatibility and Upgrade Path

Existing config files remain valid; the new Boolean setting defaults to false.
The manual drag gesture, committed selection, annotation tools, copy/save
actions, and existing command-line options remain available. Smart-region click
and wheel behavior applies only when enabled on a supported platform. Companion
macOS fixes restore the previous application's focus when appropriate after
capture and keep capture controls clear of the Dock and menu bar independently
of this setting.

The initial implementation supports macOS and Windows. Other platforms compile
a no-op detector until they gain a backend.

## Prior Art and Alternatives

ShareX collects top-level and child-window rectangles before showing its region
overlay, ignores its own overlay window, highlights the first rectangle under
the pointer, and commits that rectangle into the normal selection workflow. The
proposed interaction follows that proven shape using native macOS Window Server
and Accessibility APIs, or Windows window geometry and UI Automation.

Issue #4910 includes an earlier working macOS patch that demonstrates Window
Server and Accessibility-based hover snapping. This RFC retains that native
approach but proposes a platform-neutral detector boundary, a complete
leaf-to-window candidate chain, explicit parent/child navigation, throttled
hit-testing, an explicit permission request, and an opt-in setting. The patch is
useful prior art and evidence of feasibility; it does not yet address the shared
backend contract requested by maintainers.

Computer-vision rectangle detection was discussed in #5 and #1814. It performs
poorly with borderless windows, shadows, overlapping dark surfaces, transparency,
and low contrast, and cannot expose a semantic parent hierarchy. Native metadata
is pixel-precise for normal windows and provides the element tree requested by
this interaction.

An active-window-only command is simpler but does not solve hover selection,
child elements, or the common case where the intended window is not focused.

## Drawbacks

* Element-level detection needs a macOS privacy permission.
* Accessibility and UI Automation quality depend on the target application's
  implementation. Some games, remote desktops, canvas-based interfaces, and
  custom controls may expose only a window or a coarse container.
* Platform-specific backends require separate testing and maintenance.
* A staged rollout temporarily gives supported platforms more capability than
  unsupported platforms, even though existing behavior remains unchanged.
* Multi-display coordinate conversion and partially off-screen windows require
  an explicit test matrix, especially with mixed scale factors.

## Test Plan

* Window-only fallback with Accessibility denied.
* Leaf, parent, reverse-to-child, and containing-window navigation.
* Physical upward scrolling moves toward the parent with both natural and
  traditional scrolling enabled; downward scrolling moves toward the child.
* Before a selection is committed, every wheel event is consumed by smart-region
  navigation and never changes the annotation tool size.
* Click-to-commit followed by resize, annotation, copy, and save.
* Drag-to-create a manual rectangle from inside a detected window.
* Chrome, Finder or File Explorer, native Qt, and a target that exposes few
  accessibility elements.
* Windows App and RDP content, which may expose only a coarse remote surface.
* Single Retina display, multiple equal-scale displays, mixed scaling, and a
  partially off-screen window.
* Accessibility target that is slow, closes during hit-testing, or refuses the
  requested attributes.
* Feature disabled and unsupported-platform builds.

## Errata

N/A
