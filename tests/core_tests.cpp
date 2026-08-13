#include "windowmark/core/Coordinator.h"
#include "windowmark/core/DrawerState.h"
#include "windowmark/core/LayoutEngine.h"
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
    void Stop() noexcept override { sink_ = {}; }
    std::vector<WindowInfo> EnumerateWindows() override { return windows; }
    std::optional<WindowInfo> QueryWindow(WindowId id) override {
        for (const auto& w : windows) {
            if (w.id == id) return w;
        }
        return std::nullopt;
    }
    bool ActivateWindow(WindowId id) override {
        activated = id;
        return true;
    }
    void Emit(WindowEvent e) { if (sink_) sink_(e); }

    std::vector<WindowInfo> windows;
    WindowId activated{};
    EventSink sink_;
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

    // A row overlay is still allotted the full thickness; the spare space is what a
    // hovered tab expands into.
    WindowInfo host = Make(1, "code.exe", "A", 0);
    host.maximized = true;
    host.frame = host.workArea;
    const auto bounds = LayoutEngine::ComputeOverlayBounds(host, 3, Placement::Bottom, settings.drawer);
    CHECK(bounds.height() == settings.drawer.thickness);
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
    TestSettingsHotUpdate();
    TestSelectionFiltering();
    TestSelectionSettingsPersistence();
    TestRowPlacementMetrics();
    TestLayout();
    TestDrawerState();
    std::cout << "WindowMark core tests passed.\n";
    return 0;
}
