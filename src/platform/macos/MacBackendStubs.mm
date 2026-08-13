// macOS backend boundary intentionally exists in the first revision.
// Future implementation should map the platform-neutral interfaces to:
// - NSWorkspace / CGWindowList for window discovery
// - Accessibility API for activation and window metadata
// - NSPanel/NSWindow for bookmark overlays
// - ScreenCaptureKit / CGWindow image APIs for previews
//
// Keep Cocoa/CoreGraphics types in this platform directory; never leak them into core/.
