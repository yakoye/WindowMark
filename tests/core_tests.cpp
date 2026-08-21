#include "windowmark/core/Coordinator.h"
#include "windowmark/core/DrawerState.h"
#include "windowmark/core/LayoutEngine.h"
#include "windowmark/core/PinRegistry.h"
#include "windowmark/core/Settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <unordered_map>

using namespace windowmark;

// assert() expands to nothing under NDEBUG, so a Release test binary would drop
// both the check and any call written inside it. CHECK always evaluates its
// expression, in every configuration.
#define CHECK(...)                                                     \
    do {                                                               \
        if (!(__VA_ARGS__)) {                                          \
            std::fprintf(stderr, "CHECK failed: %s\n  at %s:%d\n",     \
                         #__VA_ARGS__, __FILE__, __LINE__);            \
            std::fflush(stderr);                                       \
            std::exit(1);                                              \
        }                                                              \
    } while (false)

namespace {

class MockWindowBackend final : public IWindowBackend {
public:
    bool Start(EventSink sink) override {
        sink_ = std::move(sink);
        return true;
    }
    void SetGeometrySink(GeometrySink sink) override { geometrySink_ = std::move(sink); }
    void Stop() noexcept override { sink_ = {}; geometrySink_ = {}; }
    std::vector<WindowInfo> EnumerateWindows() override { return windows; }
    std::optional<WindowInfo> QueryWindow(WindowId id) override {
        ++fullQueries;
        for (const auto& w : windows) {
            if (w.id == id) return w;
        }
        return std::nullopt;
    }
    std::optional<Rect> QueryFrame(WindowId id) override {
        ++frameQueries;
        for (const auto& w : windows) {
            if (w.id == id) return w.frame;
        }
        return std::nullopt;
    }
    bool ActivateWindow(WindowId id) override {
        activated = id;
        return true;
    }
    void Emit(WindowEvent e) { if (sink_) sink_(e); }
    void EmitGeometry(WindowId id, const Rect& frame) {
        if (geometrySink_) geometrySink_(id, frame);
    }

    std::vector<WindowInfo> windows;
    WindowId activated{};
    // A move must not go down the expensive path. Counted rather than asserted on shape,
    // because the whole point is how often each is called.
    int fullQueries{0};
    int frameQueries{0};
    EventSink sink_;
    GeometrySink geometrySink_;
};

class MockBorderBackend final : public IBorderBackend {
public:
    bool Start(const Settings& settings) override {
        settings_ = settings;
        started = true;
        return true;
    }
    void Apply(const std::vector<BorderModel>& models) override { last = models; ++applyCount; }
    void MoveBorder(WindowId id, const Rect& frame) override { moves.emplace_back(id, frame); }
    void UpdateSettings(const Settings& settings) override { settings_ = settings; }
    void Stop() noexcept override { started = false; last.clear(); }

    Settings settings_{};
    std::vector<BorderModel> last;
    std::vector<std::pair<WindowId, Rect>> moves;
    int applyCount{0};
    bool started{false};
};

class MockOverlayBackend final : public IOverlayBackend {
public:
    bool Start(const Settings& settings, OverlayCallbacks callbacks) override {
        settings_ = settings;
        callbacks_ = std::move(callbacks);
        return true;
    }
    void Apply(const std::vector<OverlayModel>& models) override { last = models; }
    void UpdateSettings(const Settings& settings) override {
        settings_ = settings;
        ++settingsUpdates;
    }
    void Stop() noexcept override { last.clear(); }

    Settings settings_;
    OverlayCallbacks callbacks_;
    std::vector<OverlayModel> last;
    int settingsUpdates{0};
};

class MockPreviewBackend final : public IPreviewBackend {
public:
    bool Start(const PreviewSettings&) override { return true; }
    void Show(const PreviewRequest& request) override { last = request; visible = true; }
    void UpdateSettings(const PreviewSettings& settings) override {
        settings_ = settings;
        ++settingsUpdates;
    }
    void Hide() noexcept override { visible = false; }
    void Stop() noexcept override { visible = false; }

    PreviewSettings settings_{};
    PreviewRequest last{};
    bool visible{false};
    int settingsUpdates{0};
};

WindowInfo Make(WindowId id, std::string group, std::string title, int x, bool active = false) {
    return WindowInfo{
        id,
        std::move(group),
        "Code",
        std::move(title),
        Rect{x, 100, x + 700, 800},
        Rect{0, 0, 2560, 1440},
        true,
        false,
        false,
        active,
    };
}

void TestGroupingAndSelfState() {
    Settings settings;
    MockWindowBackend windows;
    MockOverlayBackend overlays;
    MockPreviewBackend previews;

    windows.windows = {
        Make(1, "code.exe", "Grace", 100, true),
        Make(2, "code.exe", "PCIe", 850),
        Make(3, "code.exe", "YeBoard", 1600),
        Make(9, "other.exe", "Single", 300),
    };

    Coordinator coordinator(settings, windows, overlays, previews);
    CHECK(coordinator.Start());
    CHECK(overlays.last.size() == 3);

    for (const auto& model : overlays.last) {
        CHECK(model.items.size() == 3);
        int selfCount = 0;
        int activeCount = 0;
        for (const auto& item : model.items) {
            selfCount += item.isSelf ? 1 : 0;
            activeCount += item.isActive ? 1 : 0;
        }
        CHECK(selfCount == 1);
        CHECK(activeCount == 1);
    }

    const auto colorBefore = overlays.last.front().items[1].color;
    windows.windows[1].frame.left += 20;
    windows.windows[1].frame.right += 20;
    windows.Emit({WindowEventKind::GeometryChanged, 2});
    const auto colorAfter = overlays.last.front().items[1].color;
    CHECK(colorBefore.r == colorAfter.r && colorBefore.g == colorAfter.g && colorBefore.b == colorAfter.b);

    overlays.callbacks_.onActivate(3);
    CHECK(windows.activated == 3);

    coordinator.Stop();
}

// Overlays are owned popups of windows in other processes, and Windows does not keep
// cross-process owner/owned z-order in sync, so a background strip could float above an
// unrelated foreground window. The default is therefore to build a model for every host
// but only mark the active one visible.
void TestActiveWindowOnlyVisibility() {
    Settings settings;
    CHECK(settings.drawer.activeWindowOnly);

    MockWindowBackend windows;
    MockOverlayBackend overlays;
    MockPreviewBackend previews;
    windows.windows = {
        Make(1, "code.exe", "Grace", 100, true),
        Make(2, "code.exe", "PCIe", 850),
        Make(3, "code.exe", "YeBoard", 1600),
    };

    Coordinator coordinator(settings, windows, overlays, previews);
    CHECK(coordinator.Start());
    CHECK(overlays.last.size() == 3);

    const auto visibleHost = [&overlays]() -> WindowId {
        WindowId host = 0;
        int count = 0;
        for (const auto& model : overlays.last) {
            if (model.visible) {
                host = model.hostWindowId;
                ++count;
            }
        }
        return count == 1 ? host : 0;
    };

    CHECK(visibleHost() == 1);

    // Focus moves to another window in the same group: exactly one strip follows it.
    windows.windows[0].active = false;
    windows.windows[2].active = true;
    windows.Emit({WindowEventKind::ActiveChanged, 3});
    CHECK(visibleHost() == 3);

    // With the policy off, every non-minimized host shows its own strip again.
    Settings always = settings;
    always.drawer.activeWindowOnly = false;
    MockWindowBackend windows2;
    MockOverlayBackend overlays2;
    MockPreviewBackend previews2;
    windows2.windows = windows.windows;
    Coordinator permissive(always, windows2, overlays2, previews2);
    CHECK(permissive.Start());
    CHECK(overlays2.last.size() == 3);
    for (const auto& model : overlays2.last) CHECK(model.visible);
    permissive.Stop();

    coordinator.Stop();
}

// Renaming is session-only by design (a generic OS window has no reliable cross-session
// identity), so the contract is: it overrides the title while the window lives, survives
// refreshes, clears on empty, and disappears with the window.
void TestCustomLabels() {
    Settings settings;
    MockWindowBackend windows;
    MockOverlayBackend overlays;
    MockPreviewBackend previews;
    windows.windows = {
        Make(1, "code.exe", "Grace", 100, true),
        Make(2, "code.exe", "PCIe", 850),
    };

    Coordinator coordinator(settings, windows, overlays, previews);
    CHECK(coordinator.Start());

    const auto labelOf = [&overlays](WindowId id) -> std::string {
        for (const auto& model : overlays.last) {
            for (const auto& item : model.items) {
                if (item.targetWindowId == id) return item.label;
            }
        }
        return {};
    };

    CHECK(labelOf(2) == "PCIe");
    CHECK(coordinator.CustomLabel(2).empty());
    CHECK(coordinator.DefaultLabel(2) == "PCIe");

    coordinator.SetCustomLabel(2, "\xE6\x8E\xA5\xE5\x8F\xA3");  // UTF-8 "接口"
    CHECK(coordinator.CustomLabel(2) == "\xE6\x8E\xA5\xE5\x8F\xA3");
    CHECK(labelOf(2) == "\xE6\x8E\xA5\xE5\x8F\xA3");
    CHECK(labelOf(1) == "Grace");

    // A geometry refresh rebuilds every model; the custom name must not be lost.
    windows.windows[1].frame.left += 20;
    windows.Emit({WindowEventKind::GeometryChanged, 2});
    CHECK(labelOf(2) == "\xE6\x8E\xA5\xE5\x8F\xA3");

    // Clearing falls back to the live window title.
    coordinator.SetCustomLabel(2, "");
    CHECK(coordinator.CustomLabel(2).empty());
    CHECK(labelOf(2) == "PCIe");

    // A name attached to a window that goes away must not linger on a recycled id.
    coordinator.SetCustomLabel(2, "gone");
    windows.windows.pop_back();
    windows.Emit({WindowEventKind::StructureChanged, 0});
    CHECK(coordinator.CustomLabel(2).empty());

    coordinator.Stop();
}

// A collapsed tab shows the first few characters of a title, so a title whose first
// characters draw nothing produces a blank tab. Real case: a Chrome page carrying fifty
// zero-width code points before its first glyph.
void TestTitleSanitising() {
    // Ordinary titles come through untouched, including CJK and emoji.
    CHECK(SanitizeTitle("PCIe Report") == "PCIe Report");
    CHECK(SanitizeTitle("\xE6\x8E\xA5\xE5\x8F\xA3") == "\xE6\x8E\xA5\xE5\x8F\xA3");
    CHECK(SanitizeTitle("\xF0\x9F\x93\x8A x") == "\xF0\x9F\x93\x8A x");  // U+1F4CA, 4-byte
    CHECK(SanitizeTitle("").empty());

    // Each invisible class is stripped: ZWNJ, BOM, invisible separator, bidi isolate,
    // soft hyphen, LRM.
    CHECK(SanitizeTitle("\xE2\x80\x8C\xEF\xBB\xBF\xE2\x81\xA3""ab") == "ab");
    CHECK(SanitizeTitle("\xE2\x81\xA6""x\xE2\x81\xA9") == "x");
    CHECK(SanitizeTitle("a\xC2\xAD""b") == "ab");
    CHECK(SanitizeTitle("\xE2\x80\x8E\xE2\x80\x8F""hi") == "hi");

    // The real shape of the bug: the first three characters were all invisible, so a
    // three-character label rendered empty. After stripping, the label starts at a glyph.
    const std::string chrome =
        "\xE2\x80\x8C\xEF\xBB\xBF\xE2\x81\xA2\xE2\x81\xA1\xE2\x81\xA2\xE2\x81\xA4"
        "5.haps";
    CHECK(SanitizeTitle(chrome) == "5.haps");

    // Leading whitespace would take a visible character's place just the same.
    CHECK(SanitizeTitle("  \t x ") == "x");
    CHECK(SanitizeTitle("\xE2\x80\x8B  Report") == "Report");

    // A title that is nothing but invisibles has no glyph to offer; the caller falls back
    // to the app name, which is what an empty title already means.
    CHECK(SanitizeTitle("\xE2\x80\x8C\xE2\x80\x8D\xEF\xBB\xBF").empty());

    // Malformed UTF-8 must not swallow the rest of the string.
    CHECK(SanitizeTitle("a\xFF""b") == "a\xFF""b");
    CHECK(SanitizeTitle("a\xE4\xB8") == "a\xE4\xB8");  // truncated 3-byte sequence
}

// A window that only moved must not be re-queried in full. One 80-step drag of an Excel
// window delivers 274 location events, and the full query costs ~0.7ms of cross-process
// calls each - to re-derive a class name, process path and title that a move cannot have
// changed.
void TestMoveDoesNotRequery() {
    Settings settings;
    MockWindowBackend windows;
    MockOverlayBackend overlays;
    MockPreviewBackend previews;
    windows.windows = {
        Make(1, "code.exe", "A", 100, true),
        Make(2, "code.exe", "B", 900),
    };

    Coordinator coordinator(settings, windows, overlays, previews);
    CHECK(coordinator.Start());

    windows.fullQueries = 0;
    windows.frameQueries = 0;

    // Ten moves of a tracked window.
    for (int i = 0; i < 10; ++i) {
        windows.windows[1].frame.left += 7;
        windows.windows[1].frame.right += 7;
        windows.Emit({WindowEventKind::GeometryChanged, 2});
    }
    CHECK(windows.frameQueries == 10);
    CHECK(windows.fullQueries == 0);

    // A resize can cross a monitor edge or change the maximized state, so it still has to
    // take the full path.
    windows.fullQueries = 0;
    windows.windows[1].frame.right += 50;
    windows.Emit({WindowEventKind::GeometryChanged, 2});
    CHECK(windows.fullQueries == 1);

    // An untracked id has to be looked at properly - it may have just become eligible.
    windows.fullQueries = 0;
    windows.Emit({WindowEventKind::GeometryChanged, 999});
    CHECK(windows.fullQueries == 1);

    coordinator.Stop();
}

// The settings UI edits a copy and hands it back; backends must see it without a restart.
void TestSettingsHotUpdate() {
    Settings settings;
    MockWindowBackend windows;
    MockOverlayBackend overlays;
    MockPreviewBackend previews;
    windows.windows = {
        Make(1, "code.exe", "Grace", 100, true),
        Make(2, "code.exe", "PCIe", 850),
    };

    Coordinator coordinator(settings, windows, overlays, previews);
    CHECK(coordinator.Start());
    CHECK(overlays.settingsUpdates == 0);
    CHECK(coordinator.CurrentSettings().drawer.transparency == 0);

    Settings edited = coordinator.CurrentSettings();
    edited.drawer.transparency = 35;
    edited.drawer.collapsedExtent = 44;
    edited.preview.delayMs = 900;
    coordinator.UpdateSettings(edited);

    CHECK(coordinator.CurrentSettings().drawer.transparency == 35);
    CHECK(overlays.settingsUpdates == 1);
    CHECK(overlays.settings_.drawer.collapsedExtent == 44);
    CHECK(previews.settingsUpdates == 1);
    CHECK(previews.settings_.delayMs == 900);
    // Models are re-applied so the new geometry takes effect immediately.
    CHECK(overlays.last.size() == 2);

    coordinator.Stop();
}

// Borders are a separate feature sharing only window tracking: their own switch, every
// top-level window (not just grouped ones), and an unthrottled move path so they do not
// trail a drag.
void TestPinRegistry() {
    PinRegistry pins;
    CHECK(pins.Empty());

    // A window that was ordinary before we pinned it.
    CHECK(pins.Add(1, false));
    CHECK(pins.Contains(1));
    CHECK(pins.Size() == 1);

    // A window that was already always-on-top on its own.
    CHECK(pins.Add(2, true));

    // Pinning twice must not overwrite the recorded original state. If it did, pin, pin
    // again, unpin would leave the window in the state it had after the first pin rather
    // than the one it started in - which for window 2 means silently clearing an
    // always-on-top setting the user made themselves.
    CHECK(!pins.Add(2, false));
    const auto restored = pins.Remove(2);
    CHECK(restored.has_value());
    CHECK(*restored == true);
    CHECK(!pins.Contains(2));

    // Removing something that was never pinned reports nothing rather than guessing.
    CHECK(!pins.Remove(99).has_value());

    // Insertion order is stable, so the tray menu does not reshuffle between openings.
    CHECK(pins.Add(3, false));
    CHECK(pins.Add(4, false));
    const auto snapshot = pins.Snapshot();
    CHECK(snapshot.size() == 3);
    CHECK(snapshot[0].windowId == 1);
    CHECK(snapshot[1].windowId == 3);
    CHECK(snapshot[2].windowId == 4);

    // Drain hands back everything needed to restore, and empties the registry.
    const auto drained = pins.Drain();
    CHECK(drained.size() == 3);
    CHECK(pins.Empty());
    CHECK(pins.Drain().empty());
}

void TestBorders() {
    Settings settings;
    CHECK(!settings.border.enabled);  // opt-in

    MockWindowBackend windows;
    MockOverlayBackend overlays;
    MockPreviewBackend previews;
    MockBorderBackend borders;
    windows.windows = {
        Make(1, "code.exe", "Grace", 100, true),
        Make(2, "code.exe", "PCIe", 850),
        Make(9, "solo.exe", "Alone", 300),   // single-window app: no bookmarks, but a border
    };

    Coordinator coordinator(settings, windows, overlays, previews, &borders);
    CHECK(coordinator.Start());
    CHECK(borders.started);
    // Disabled by default, so nothing is drawn even though the backend is running.
    CHECK(borders.last.empty());

    Settings enabled = coordinator.CurrentSettings();
    enabled.border.enabled = true;
    coordinator.UpdateSettings(enabled);

    // Every tracked window, including the one that never gets a bookmark strip.
    CHECK(borders.last.size() == 3);
    const auto find = [&borders](WindowId id) -> const BorderModel* {
        for (const auto& m : borders.last) {
            if (m.windowId == id) return &m;
        }
        return nullptr;
    };
    CHECK(find(9) != nullptr);
    CHECK(find(1) != nullptr && find(1)->active);
    CHECK(find(2) != nullptr && !find(2)->active);

    // Focus change repaints borders even though bookmarks are switched off.
    coordinator.SetOverlayEnabled(false);
    windows.windows[0].active = false;
    windows.windows[1].active = true;
    windows.Emit({WindowEventKind::ActiveChanged, 2});
    CHECK(find(2) != nullptr && find(2)->active);
    CHECK(find(1) != nullptr && !find(1)->active);

    // The unthrottled sink reaches the backend as a move, not a rebuild.
    const int appliesBefore = borders.applyCount;
    windows.EmitGeometry(2, Rect{10, 20, 110, 120});
    CHECK(borders.moves.size() == 1);
    CHECK(borders.moves.back().first == 2);
    CHECK(borders.moves.back().second.left == 10);
    CHECK(borders.applyCount == appliesBefore);

    // Turning borders off clears them.
    Settings off = coordinator.CurrentSettings();
    off.border.enabled = false;
    coordinator.UpdateSettings(off);
    CHECK(borders.last.empty());

    coordinator.Stop();
    CHECK(!borders.started);
}

void TestSelectionFiltering() {
    Settings settings;
    MockWindowBackend windows;
    MockOverlayBackend overlays;
    MockPreviewBackend previews;
    windows.windows = {
        Make(1, "code.exe", "Grace", 100, true),
        Make(2, "code.exe", "PCIe", 850),
        Make(3, "code.exe", "YeBoard", 1600),
        Make(10, "chrome.exe", "ChatGPT", 100),
        Make(11, "chrome.exe", "Jira", 850),
    };

    Coordinator coordinator(settings, windows, overlays, previews);
    CHECK(coordinator.Start());
    CHECK(overlays.last.size() == 5);

    auto selection = coordinator.SelectionSnapshot();
    CHECK(selection.size() == 2);

    for (auto& app : selection) {
        if (app.groupKey == "code.exe") {
            CHECK(app.windows.size() == 3);
            app.windows[1].enabled = false;
        }
    }
    coordinator.ApplySelection(selection);

    int codeHosts = 0;
    int chromeHosts = 0;
    for (const auto& model : overlays.last) {
        if (model.items.front().targetWindowId <= 3) {
            ++codeHosts;
            CHECK(model.items.size() == 2);
            CHECK(model.hostWindowId != 2);
        } else {
            ++chromeHosts;
        }
    }
    CHECK(codeHosts == 2);
    CHECK(chromeHosts == 2);

    selection = coordinator.SelectionSnapshot();
    for (auto& app : selection) {
        if (app.groupKey == "chrome.exe") app.enabled = false;
    }
    coordinator.ApplySelection(selection);
    CHECK(coordinator.CurrentSettings().selection.disabledAppKeys.size() == 1);
    for (const auto& model : overlays.last) {
        for (const auto& item : model.items) CHECK(item.targetWindowId <= 3);
    }

    coordinator.Stop();
}

void TestSelectionSettingsPersistence() {
    Settings settings;
    settings.selection.disabledAppKeys = {
        "c:\\program files\\google\\chrome\\application\\chrome.exe",
        "/Applications/Visual Studio Code.app",
    };

    const auto path = std::filesystem::temp_directory_path() / "windowmark-settings-test.conf";
    CHECK(Settings::Save(path, settings));
    const Settings loaded = Settings::LoadOrCreate(path);
    CHECK(loaded.selection.disabledAppKeys.size() == 2);
    CHECK(loaded.selection.disabledAppKeys[0] == settings.selection.disabledAppKeys[0] ||
           loaded.selection.disabledAppKeys[1] == settings.selection.disabledAppKeys[0]);
    CHECK(loaded.selection.disabledAppKeys[0] == settings.selection.disabledAppKeys[1] ||
           loaded.selection.disabledAppKeys[1] == settings.selection.disabledAppKeys[1]);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void TestDrawerState() {
    DrawerState drawer(52, 180, 100);
    drawer.Reset(3);
    CHECK(drawer.Extents().size() == 3);
    CHECK(drawer.Extents()[0] == 52.0F);

    drawer.SetHovered(1, 1000);
    CHECK(drawer.HoveredIndex() == 1);
    CHECK(drawer.IsAnimating());
    CHECK(drawer.Tick(1050));
    CHECK(drawer.Extents()[1] > 52.0F && drawer.Extents()[1] < 180.0F);
    CHECK(drawer.Extents()[0] == 52.0F);
    CHECK(drawer.Tick(1100));
    CHECK(!drawer.IsAnimating());
    CHECK(drawer.Extents()[1] == 180.0F);

    drawer.SetHovered(-1, 1200);
    CHECK(drawer.Tick(1300));
    CHECK(drawer.Extents()[1] == 52.0F);
}

// Row placements (maximized hosts) measure a tab's extent as width and rest at part of
// their thickness, so they cannot reuse the side numbers.
void TestRowPlacementMetrics() {
    Settings settings;

    const auto side = LayoutEngine::MetricsFor(Placement::Left, settings.drawer);
    CHECK(!LayoutEngine::IsRowPlacement(Placement::Left));
    CHECK(side.collapsedExtent == settings.drawer.collapsedExtent);
    CHECK(side.expandedExtent == settings.drawer.expandedExtent);
    CHECK(side.restThickness == side.fullThickness);
    // Side tabs are all one height - the active one is told apart by reaching further
    // out - so the row-only active height must not shrink them.
    CHECK(side.activeThickness == side.fullThickness);

    const auto row = LayoutEngine::MetricsFor(Placement::Bottom, settings.drawer);
    CHECK(LayoutEngine::IsRowPlacement(Placement::Bottom));
    CHECK(LayoutEngine::IsRowPlacement(Placement::Top));
    CHECK(row.collapsedExtent == settings.drawer.bottomCollapsedExtent);
    CHECK(row.expandedExtent == settings.drawer.bottomExpandedExtent);
    CHECK(row.collapsedExtent != side.collapsedExtent);
    // Rest at half thickness by default, leaving room to grow upward on hover.
    CHECK(row.restThickness == settings.drawer.thickness / 2);
    CHECK(row.restThickness < row.fullThickness);

    Settings custom = settings;
    custom.drawer.bottomCollapsedThickness = 12;
    CHECK(LayoutEngine::MetricsFor(Placement::Bottom, custom.drawer).restThickness == 12);

    // The active row tab has its own height, independent of `thickness`. It used to be
    // hard-wired to `thickness`, so changing it meant changing every tab and the strip.
    CHECK(settings.drawer.bottomActiveThickness == 23);
    CHECK(row.activeThickness == 23);
    CHECK(row.restThickness < row.activeThickness);
    CHECK(row.activeThickness < row.fullThickness);

    // 0 means "same as thickness", which is what the behaviour was before the setting.
    Settings legacy = settings;
    legacy.drawer.bottomActiveThickness = 0;
    const auto legacyRow = LayoutEngine::MetricsFor(Placement::Bottom, legacy.drawer);
    CHECK(legacyRow.activeThickness == legacy.drawer.thickness);

    // Clamped: never under the resting height (the active tab would sink below its
    // neighbours) and never over the full thickness (the strip is only that tall).
    Settings tooSmall = settings;
    tooSmall.drawer.bottomActiveThickness = 4;
    const auto smallRow = LayoutEngine::MetricsFor(Placement::Bottom, tooSmall.drawer);
    CHECK(smallRow.activeThickness == smallRow.restThickness);
    Settings tooBig = settings;
    tooBig.drawer.bottomActiveThickness = 500;
    CHECK(LayoutEngine::MetricsFor(Placement::Bottom, tooBig.drawer).activeThickness ==
          tooBig.drawer.thickness);

    // The strip is sized to the active tab, not to `thickness`: nothing grows past it,
    // so the extra would be transparent padding hanging over the window.
    WindowInfo host = Make(1, "code.exe", "A", 0);
    host.maximized = true;
    host.frame = host.workArea;
    const auto bounds = LayoutEngine::ComputeOverlayBounds(host, 3, Placement::Bottom, settings.drawer);
    CHECK(bounds.height() == 23);
    const auto legacyBounds =
        LayoutEngine::ComputeOverlayBounds(host, 3, Placement::Bottom, legacy.drawer);
    CHECK(legacyBounds.height() == legacy.drawer.thickness);
}

void TestLayout() {
    Settings settings;

    // The default is a fixed bottom edge: Auto moves the strip between the left and right
    // sides as a window is dragged, which makes it hard to find. An explicit placement is
    // always honoured as-is.
    CHECK(settings.drawer.placement == Placement::Bottom);
    WindowInfo normal = Make(1, "code.exe", "A", 300);
    CHECK(LayoutEngine::ResolvePlacement(normal, settings.drawer) == Placement::Bottom);

    Settings autoPlaced = settings;
    autoPlaced.drawer.placement = Placement::Auto;
    auto placement = LayoutEngine::ResolvePlacement(normal, autoPlaced.drawer);
    CHECK(placement == Placement::Left);

    normal.maximized = true;
    normal.frame = normal.workArea;
    placement = LayoutEngine::ResolvePlacement(normal, autoPlaced.drawer);
    CHECK(placement == Placement::Bottom);

    const auto bounds = LayoutEngine::ComputeOverlayBounds(normal, 3, placement, settings.drawer);
    CHECK(bounds.bottom <= normal.workArea.bottom);
    CHECK(bounds.width() > 0 && bounds.height() > 0);
}

} // namespace

int main() {
    TestGroupingAndSelfState();
    TestActiveWindowOnlyVisibility();
    TestCustomLabels();
    TestTitleSanitising();
    TestMoveDoesNotRequery();
    TestSettingsHotUpdate();
    TestBorders();
    TestPinRegistry();
    TestSelectionFiltering();
    TestSelectionSettingsPersistence();
    TestRowPlacementMetrics();
    TestLayout();
    TestDrawerState();
    std::cout << "WindowMark core tests passed.\n";
    return 0;
}
