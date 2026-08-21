#include "windowmark/core/Coordinator.h"

#include "windowmark/core/LayoutEngine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace windowmark {
namespace {

constexpr std::array<Color, 12> kPastelRainbow{{
    {0.96F, 0.42F, 0.46F, 1.0F}, // coral
    {0.98F, 0.62F, 0.36F, 1.0F}, // orange
    {0.95F, 0.78F, 0.36F, 1.0F}, // amber
    {0.73F, 0.82F, 0.38F, 1.0F}, // lime
    {0.40F, 0.76F, 0.48F, 1.0F}, // green
    {0.33F, 0.78F, 0.68F, 1.0F}, // teal
    {0.35F, 0.72F, 0.86F, 1.0F}, // cyan
    {0.38F, 0.57F, 0.88F, 1.0F}, // blue
    {0.52F, 0.48F, 0.88F, 1.0F}, // indigo
    {0.70F, 0.46F, 0.86F, 1.0F}, // violet
    {0.88F, 0.46F, 0.72F, 1.0F}, // pink
    {0.66F, 0.68F, 0.74F, 1.0F}, // neutral
}};

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

Coordinator::Coordinator(
    Settings settings,
    IWindowBackend& windowBackend,
    IOverlayBackend& overlayBackend,
    IPreviewBackend& previewBackend,
    IBorderBackend* borderBackend,
    IPinBackend* pinBackend)
    : settings_(std::move(settings)),
      windowsBackend_(windowBackend),
      overlaysBackend_(overlayBackend),
      previewBackend_(previewBackend),
      borderBackend_(borderBackend),
      pinBackend_(pinBackend) {}

bool Coordinator::Start() {
    if (started_) {
        return true;
    }

    OverlayCallbacks callbacks;
    callbacks.onActivate = [this](WindowId id) {
        previewBackend_.Hide();
        windowsBackend_.ActivateWindow(id);
    };
    callbacks.onPreview = [this](const PreviewRequest& request) {
        if (settings_.preview.enabled) {
            previewBackend_.Show(request);
        }
    };
    callbacks.onPreviewHide = [this]() { previewBackend_.Hide(); };
    callbacks.onRename = onRename_;
    callbacks.onOpenSettings = onOpenSettings_;

    if (!previewBackend_.Start(settings_.preview)) {
        return false;
    }
    if (!overlaysBackend_.Start(settings_, std::move(callbacks))) {
        previewBackend_.Stop();
        return false;
    }
    if (borderBackend_ && !borderBackend_->Start(settings_)) {
        overlaysBackend_.Stop();
        previewBackend_.Stop();
        return false;
    }

    if (pinBackend_) {
        PinCallbacks pinCallbacks;
        pinCallbacks.onTogglePin = [this](WindowId id) { TogglePin(id); };
        pinCallbacks.onUnpinAll = [this]() { UnpinAll(); };
        if (!pinBackend_->Start(settings_, std::move(pinCallbacks))) {
            if (borderBackend_) borderBackend_->Stop();
    if (pinBackend_) pinBackend_->Stop();
            overlaysBackend_.Stop();
            previewBackend_.Stop();
            return false;
        }
    }

    windowsBackend_.SetExcludedClasses(settings_.tracking.excludeClasses);
    if (!windowsBackend_.Start([this](const WindowEvent& event) { OnWindowEvent(event); })) {
        if (borderBackend_) borderBackend_->Stop();
    if (pinBackend_) pinBackend_->Stop();
        overlaysBackend_.Stop();
        previewBackend_.Stop();
        return false;
    }

    // Borders track the window edge, so they take the unthrottled path and only move -
    // repainting is left to the normal event flow.
    //
    // Bookmarks deliberately do *not*. Putting them on this path too was tried and
    // measured: it added a SetWindowPos per location event (274 in one 80-step drag)
    // without removing any of the throttled work, so CPU went up and the dragged window's
    // smoothness did not change. The strip sits outside the window, where 30fps is fine.
    if (borderBackend_) {
        windowsBackend_.SetGeometrySink([this](WindowId id, const Rect& frame) {
            if (!started_ || !settings_.border.enabled) return;
            borderBackend_->MoveBorder(id, frame);
        });
    }

    started_ = true;
    RefreshAll();
    return true;
}

void Coordinator::Stop() noexcept {
    if (!started_) {
        return;
    }
    // Restore before anything else shuts down. Leaving windows pinned after the app
    // exits strands them in front of everything with no way left to release them -
    // the only tool that could has just gone away.
    UnpinAll();
    previewBackend_.Hide();
    overlaysBackend_.Apply({});
    if (borderBackend_) borderBackend_->Apply({});
    windowsBackend_.SetGeometrySink({});
    windowsBackend_.Stop();
    overlaysBackend_.Stop();
    previewBackend_.Stop();
    if (borderBackend_) borderBackend_->Stop();
    if (pinBackend_) pinBackend_->Stop();
    windows_.clear();
    disabledWindowIds_.clear();
    customLabels_.clear();
    started_ = false;
}

void Coordinator::SetOverlayEnabled(bool enabled) {
    settings_.drawer.enabled = enabled;
    if (!enabled) {
        previewBackend_.Hide();
        overlaysBackend_.Apply({});
        return;
    }
    ApplyModels();
}

std::vector<AppSelectionModel> Coordinator::SelectionSnapshot() const {
    std::unordered_map<std::string, std::vector<const WindowInfo*>> groups;
    for (const auto& [_, window] : windows_) {
        if (!window.groupKey.empty()) {
            groups[window.groupKey].push_back(&window);
        }
    }

    std::vector<AppSelectionModel> result;
    result.reserve(groups.size());
    for (auto& [groupKey, members] : groups) {
        // A single-window app cannot show a same-app bookmark strip, so keep the
        // native selector focused on groups where the feature is meaningful.
        if (members.size() < 2) continue;

        std::sort(members.begin(), members.end(), [this](const WindowInfo* a, const WindowInfo* b) {
            const auto ao = stableOrder_.find(a->id);
            const auto bo = stableOrder_.find(b->id);
            const std::size_t av = ao == stableOrder_.end() ? 0 : ao->second;
            const std::size_t bv = bo == stableOrder_.end() ? 0 : bo->second;
            return av < bv;
        });

        AppSelectionModel app;
        app.groupKey = groupKey;
        app.appName = members.empty() ? groupKey : members.front()->appName;
        app.enabled = IsAppEnabled(groupKey);
        app.windows.reserve(members.size());
        for (const WindowInfo* window : members) {
            app.windows.push_back(WindowSelectionModel{
                window->id,
                window->title.empty() ? window->appName : window->title,
                IsWindowEnabled(window->id),
            });
        }
        result.push_back(std::move(app));
    }

    std::sort(result.begin(), result.end(), [](const AppSelectionModel& a, const AppSelectionModel& b) {
        const std::string an = LowerAscii(a.appName);
        const std::string bn = LowerAscii(b.appName);
        if (an != bn) return an < bn;
        return a.groupKey < b.groupKey;
    });
    return result;
}

void Coordinator::ApplySelection(const std::vector<AppSelectionModel>& selection) {
    for (const auto& app : selection) {
        auto disabledIt = std::find(
            settings_.selection.disabledAppKeys.begin(),
            settings_.selection.disabledAppKeys.end(),
            app.groupKey);

        if (app.enabled) {
            if (disabledIt != settings_.selection.disabledAppKeys.end()) {
                settings_.selection.disabledAppKeys.erase(disabledIt);
            }
        } else if (disabledIt == settings_.selection.disabledAppKeys.end()) {
            settings_.selection.disabledAppKeys.push_back(app.groupKey);
        }

        for (const auto& window : app.windows) {
            if (window.enabled) {
                disabledWindowIds_.erase(window.windowId);
            } else if (windows_.contains(window.windowId)) {
                disabledWindowIds_.insert(window.windowId);
            }
        }
    }

    std::sort(settings_.selection.disabledAppKeys.begin(), settings_.selection.disabledAppKeys.end());
    settings_.selection.disabledAppKeys.erase(
        std::unique(settings_.selection.disabledAppKeys.begin(), settings_.selection.disabledAppKeys.end()),
        settings_.selection.disabledAppKeys.end());

    previewBackend_.Hide();
    ApplyModels();
}

void Coordinator::UpdateSettings(Settings settings) {
    // Read before the assignment: turning pinning off has to release the windows it is
    // holding. Otherwise the switch that could let them go is the one that just went away,
    // and the user is left with windows stuck in front of everything.
    const bool pinningWasOn = settings_.pin.enabled;
    settings_ = std::move(settings);
    if (!started_) return;

    if (pinningWasOn && !settings_.pin.enabled) UnpinAll();

    previewBackend_.Hide();
    previewBackend_.UpdateSettings(settings_.preview);
    overlaysBackend_.UpdateSettings(settings_);
    if (borderBackend_) borderBackend_->UpdateSettings(settings_);
    if (pinBackend_) pinBackend_->UpdateSettings(settings_);
    // A newly excluded class has to disappear from the tracked set, not just stop being
    // added, so re-enumerate rather than repaint what is already there.
    windowsBackend_.SetExcludedClasses(settings_.tracking.excludeClasses);
    RefreshAll();
}

void Coordinator::SetCustomLabel(WindowId id, std::string label) {
    // An empty name is how the caller asks to fall back to the window title again.
    if (label.empty()) {
        customLabels_.erase(id);
    } else {
        customLabels_[id] = std::move(label);
    }
    ApplyModels();
}

std::string Coordinator::CustomLabel(WindowId id) const {
    const auto it = customLabels_.find(id);
    return it == customLabels_.end() ? std::string{} : it->second;
}

std::string Coordinator::DefaultLabel(WindowId id) const {
    const auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    return it->second.title.empty() ? it->second.appName : it->second.title;
}

void Coordinator::SetMenuHandlers(
    std::function<void(WindowId)> onRename, std::function<void()> onOpenSettings) {
    onRename_ = std::move(onRename);
    onOpenSettings_ = std::move(onOpenSettings);
}

void Coordinator::OnWindowEvent(const WindowEvent& event) {
    switch (event.kind) {
    case WindowEventKind::StructureChanged:
        RefreshAll();
        break;
    case WindowEventKind::GeometryChanged:
        RefreshGeometry(event.windowId);
        break;
    case WindowEventKind::TitleChanged:
    case WindowEventKind::VisibilityChanged:
        RefreshOne(event.windowId);
        break;
    case WindowEventKind::ActiveChanged:
        activeWindow_ = event.windowId;
        if (activeWindow_ != 0) lastActiveWindow_ = activeWindow_;
        ApplyModels();
        ApplyBorders();
        break;
    }
}

void Coordinator::RefreshAll() {
    auto snapshot = windowsBackend_.EnumerateWindows();
    windows_.clear();
    activeWindow_ = 0;

    for (auto& window : snapshot) {
        if (!stableOrder_.contains(window.id)) {
            stableOrder_[window.id] = nextStableOrder_++;
        }
        if (!colorSlots_.contains(window.id)) {
            colorSlots_[window.id] = nextColorSlot_++;
        }
        if (window.active) {
            activeWindow_ = window.id;
            lastActiveWindow_ = window.id;
        }
        // Once, here, rather than every time a label is built: titles change far less
        // often than models are rebuilt.
        window.title = SanitizeTitle(window.title);
        windows_.emplace(window.id, std::move(window));
    }

    PruneTransientState();
    ApplyPins();
    ApplyModels();
    ApplyBorders();
}

// A window moved. That is the only thing a location event can mean, and it is by far the
// most common event there is - one 80-step drag of an Excel window delivers 274 of them.
// Going through RefreshOne for each cost about 0.7ms of cross-process calls to re-derive
// a class name, a process path, a title and a work area that a move cannot have changed.
void Coordinator::RefreshGeometry(WindowId id) {
    const auto it = windows_.find(id);
    if (it == windows_.end()) {
        // Not tracked yet - it may have just become eligible, so take the slow path once.
        RefreshOne(id);
        return;
    }
    const auto frame = windowsBackend_.QueryFrame(id);
    if (!frame.has_value()) {
        // Gone or hidden; that is a structural change, not a move.
        RefreshAll();
        return;
    }
    if (it->second.frame.left == frame->left && it->second.frame.top == frame->top &&
        it->second.frame.right == frame->right && it->second.frame.bottom == frame->bottom) {
        return;
    }

    const bool sizeChanged = it->second.frame.width() != frame->width() ||
                             it->second.frame.height() != frame->height();
    it->second.frame = *frame;
    // A resize can cross a monitor edge or flip the maximized state, both of which change
    // the layout rather than just its position, so that case still needs the full query.
    if (sizeChanged) {
        RefreshOne(id);
        return;
    }
    ApplyModels();
    ApplyBorders();
}

void Coordinator::RefreshOne(WindowId id) {
    auto updated = windowsBackend_.QueryWindow(id);
    if (!updated.has_value()) {
        RefreshAll();
        return;
    }

    if (!stableOrder_.contains(id)) {
        stableOrder_[id] = nextStableOrder_++;
    }
    if (!colorSlots_.contains(id)) {
        colorSlots_[id] = nextColorSlot_++;
    }
    if (updated->active) {
        activeWindow_ = id;
        lastActiveWindow_ = id;
    }
    updated->title = SanitizeTitle(updated->title);
    windows_[id] = std::move(*updated);
    ApplyModels();
    ApplyBorders();
}

void Coordinator::ApplyModels() {
    if (!started_) return;
    if (!settings_.drawer.enabled) {
        // Clear rather than return: the switch may have just been turned off from the
        // settings dialog, and the strips are still on screen.
        overlaysBackend_.Apply({});
        return;
    }
    overlaysBackend_.Apply(BuildModels());
}

// Borders have their own switch: turning bookmarks off must not take them with it.

void Coordinator::TogglePin(WindowId id) {
    if (!started_ || !pinBackend_ || !settings_.pin.enabled) return;
    // Only tracked windows. An excluded one has no border to show for it, so pinning it
    // would set the style with no sign that anything happened - and no entry in the tray
    // list to undo it with.
    if (!windows_.contains(id) && !pins_.Contains(id)) return;

    if (const auto was = pins_.Remove(id); was.has_value()) {
        // Restore what the window had before, which is not always "not topmost": Task
        // Manager and a lot of media players ship their own always-on-top switch, and
        // clearing it here would silently undo the user's own setting.
        pinBackend_->SetTopmost(id, *was);
        ApplyPins();
        ApplyBorders();
        return;
    }

    const auto wasTopmost = pinBackend_->SetTopmost(id, true);
    if (!wasTopmost.has_value()) return;   // window went away between menu and click
    pins_.Add(id, *wasTopmost);
    ApplyPins();
    ApplyBorders();
}

void Coordinator::UnpinAll() {
    if (!pinBackend_) return;
    for (const auto& record : pins_.Drain()) {
        pinBackend_->SetTopmost(record.windowId, record.wasTopmostBefore);
    }
    ApplyPins();
    ApplyBorders();
}

std::string Coordinator::PinnedTitle(WindowId id) const {
    const auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    if (const auto custom = customLabels_.find(id); custom != customLabels_.end()) {
        return custom->second;
    }
    return it->second.title.empty() ? it->second.appName : it->second.title;
}

void Coordinator::ApplyPins() {
    if (!started_ || !pinBackend_) return;
    pinBackend_->Apply(pins_.Snapshot());
}

void Coordinator::PrunePins() {
    if (pins_.Empty()) return;
    for (const auto& record : pins_.Snapshot()) {
        if (windows_.contains(record.windowId)) continue;
        // Gone for good. No restore: the window took its own style with it, and calling
        // SetWindowPos on a dead handle would just fail.
        pins_.Remove(record.windowId);
    }
}

void Coordinator::ApplyBorders() {
    if (!started_ || !borderBackend_) return;
    // A pinned window that has since closed must not keep a slot in the registry, and this
    // runs on every structural change, which is exactly when that becomes true.
    PrunePins();
    borderBackend_->Apply(BuildBorderModels());
}

std::vector<BorderModel> Coordinator::BuildBorderModels() const {
    std::vector<BorderModel> models;
    // Note what is *not* checked here: settings_.border.enabled. A pinned window is
    // outlined whichever way that switch is set, because the outline is the only feedback
    // that the pin worked. With borders off, this list is exactly the pinned windows.
    const bool bordersOn = settings_.border.enabled;
    if (!bordersOn && pins_.Empty()) return models;

    models.reserve(windows_.size());
    for (const auto& [id, window] : windows_) {
        // Skip hidden and minimized windows outright rather than emitting an invisible
        // model: every border costs a window plus a bitmap, and a desktop full of
        // minimized windows would pay for outlines nobody can see.
        if (!window.visible || window.minimized) continue;
        const bool pinned = pins_.Contains(id);
        if (!bordersOn && !pinned) continue;

        BorderModel model;
        model.windowId = id;
        model.frame = window.frame;
        model.active = id == activeWindow_;
        model.pinned = pinned;
        model.visible = true;
        models.push_back(model);
    }
    return models;
}

std::vector<OverlayModel> Coordinator::BuildModels() {
    std::unordered_map<std::string, std::vector<const WindowInfo*>> groups;
    groups.reserve(windows_.size());

    for (const auto& [id, window] : windows_) {
        if (!window.groupKey.empty() && IsAppEnabled(window.groupKey) && IsWindowEnabled(id)) {
            groups[window.groupKey].push_back(&window);
        }
    }

    std::vector<OverlayModel> models;
    for (auto& [groupKey, members] : groups) {
        (void)groupKey;
        if (members.size() < 2) {
            continue;
        }

        std::sort(members.begin(), members.end(), [this](const WindowInfo* a, const WindowInfo* b) {
            return stableOrder_[a->id] < stableOrder_[b->id];
        });

        for (const WindowInfo* host : members) {
            OverlayModel model;
            model.hostWindowId = host->id;
            model.hostFrame = host->frame;
            model.workArea = host->workArea;
            model.visible = host->visible && !host->minimized;
            if (settings_.drawer.activeWindowOnly) {
                model.visible = model.visible && host->id == activeWindow_;
            }
            model.placement = LayoutEngine::ResolvePlacement(*host, settings_.drawer);
            model.screenBounds = LayoutEngine::ComputeOverlayBounds(*host, members.size(), model.placement, settings_.drawer);

            model.items.reserve(members.size());
            for (const WindowInfo* member : members) {
                BookmarkItemModel item;
                item.targetWindowId = member->id;
                if (const auto custom = customLabels_.find(member->id); custom != customLabels_.end()) {
                    item.label = custom->second;
                } else {
                    item.label = member->title.empty() ? member->appName : member->title;
                }
                item.color = ColorFor(member->id);
                item.isSelf = member->id == host->id;
                item.isActive = member->id == activeWindow_;
                model.items.push_back(std::move(item));
            }
            models.push_back(std::move(model));
        }
    }

    return models;
}

Color Coordinator::ColorFor(WindowId id) {
    if (!colorSlots_.contains(id)) {
        colorSlots_[id] = nextColorSlot_++;
    }
    return kPastelRainbow[colorSlots_[id] % kPastelRainbow.size()];
}

bool Coordinator::IsAppEnabled(const std::string& groupKey) const {
    return std::find(
        settings_.selection.disabledAppKeys.begin(),
        settings_.selection.disabledAppKeys.end(),
        groupKey) == settings_.selection.disabledAppKeys.end();
}

bool Coordinator::IsWindowEnabled(WindowId id) const {
    return !disabledWindowIds_.contains(id);
}

void Coordinator::PruneTransientState() {
    std::unordered_set<WindowId> alive;
    alive.reserve(windows_.size());
    for (const auto& [id, _] : windows_) {
        alive.insert(id);
    }

    for (auto it = stableOrder_.begin(); it != stableOrder_.end();) {
        if (!alive.contains(it->first)) {
            it = stableOrder_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = colorSlots_.begin(); it != colorSlots_.end();) {
        if (!alive.contains(it->first)) {
            it = colorSlots_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = disabledWindowIds_.begin(); it != disabledWindowIds_.end();) {
        if (!alive.contains(*it)) {
            it = disabledWindowIds_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = customLabels_.begin(); it != customLabels_.end();) {
        it = alive.contains(it->first) ? std::next(it) : customLabels_.erase(it);
    }
}

} // namespace windowmark
