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
    IPreviewBackend& previewBackend)
    : settings_(std::move(settings)),
      windowsBackend_(windowBackend),
      overlaysBackend_(overlayBackend),
      previewBackend_(previewBackend) {}

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
    if (!windowsBackend_.Start([this](const WindowEvent& event) { OnWindowEvent(event); })) {
        overlaysBackend_.Stop();
        previewBackend_.Stop();
        return false;
    }

    started_ = true;
    RefreshAll();
    return true;
}

void Coordinator::Stop() noexcept {
    if (!started_) {
        return;
    }
    previewBackend_.Hide();
    overlaysBackend_.Apply({});
    windowsBackend_.Stop();
    overlaysBackend_.Stop();
    previewBackend_.Stop();
    windows_.clear();
    disabledWindowIds_.clear();
    customLabels_.clear();
    started_ = false;
}

void Coordinator::SetOverlayEnabled(bool enabled) {
    overlayEnabled_ = enabled;
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
    settings_ = std::move(settings);
    if (!started_) return;

    previewBackend_.Hide();
    previewBackend_.UpdateSettings(settings_.preview);
    overlaysBackend_.UpdateSettings(settings_);
    ApplyModels();
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
    case WindowEventKind::TitleChanged:
    case WindowEventKind::VisibilityChanged:
        RefreshOne(event.windowId);
        break;
    case WindowEventKind::ActiveChanged:
        activeWindow_ = event.windowId;
        ApplyModels();
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
        }
        windows_.emplace(window.id, std::move(window));
    }

    PruneTransientState();
    ApplyModels();
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
    }
    windows_[id] = std::move(*updated);
    ApplyModels();
}

void Coordinator::ApplyModels() {
    if (!started_ || !overlayEnabled_) {
        return;
    }
    overlaysBackend_.Apply(BuildModels());
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
