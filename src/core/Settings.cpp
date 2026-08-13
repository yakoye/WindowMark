#include "windowmark/core/Settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace windowmark {
namespace {

std::string Trim(std::string value) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool ParseBool(const std::string& value, bool fallback) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") {
        return false;
    }
    return fallback;
}

int ParseInt(const std::unordered_map<std::string, std::string>& values,
             const std::string& key,
             int fallback,
             int minValue,
             int maxValue) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    try {
        return std::clamp(std::stoi(it->second), minValue, maxValue);
    } catch (...) {
        return fallback;
    }
}

bool IsUnreserved(unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string PercentEncode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char c : value) {
        if (IsUnreserved(c)) {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string PercentDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = HexDigit(value[i + 1]);
            const int lo = HexDigit(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

std::vector<std::string> ParseEncodedList(const std::string& value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find('|', start);
        const std::string token = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!token.empty()) result.push_back(PercentDecode(token));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::string EncodeList(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << '|';
        out << PercentEncode(values[i]);
    }
    return out.str();
}

} // namespace

std::string ToString(Placement placement) {
    switch (placement) {
    case Placement::Left: return "left";
    case Placement::Right: return "right";
    case Placement::Top: return "top";
    case Placement::Bottom: return "bottom";
    case Placement::Auto:
    default: return "auto";
    }
}

Placement PlacementFromString(const std::string& value) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "left") return Placement::Left;
    if (lowered == "right") return Placement::Right;
    if (lowered == "top") return Placement::Top;
    if (lowered == "bottom") return Placement::Bottom;
    return Placement::Auto;
}

Settings Settings::LoadOrCreate(const std::filesystem::path& filePath) {
    Settings settings;
    if (!std::filesystem::exists(filePath)) {
        Save(filePath, settings);
        return settings;
    }

    std::ifstream input(filePath);
    if (!input) {
        return settings;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        values[Trim(line.substr(0, pos))] = Trim(line.substr(pos + 1));
    }

    if (const auto it = values.find("placement"); it != values.end()) {
        settings.drawer.placement = PlacementFromString(it->second);
    }
    settings.drawer.collapsedExtent = ParseInt(values, "drawer.collapsed_extent", settings.drawer.collapsedExtent, 24, 160);
    settings.drawer.expandedExtent = ParseInt(values, "drawer.expanded_extent", settings.drawer.expandedExtent, settings.drawer.collapsedExtent, 480);
    settings.drawer.thickness = ParseInt(values, "drawer.thickness", settings.drawer.thickness, 20, 80);
    settings.drawer.gap = ParseInt(values, "drawer.gap", settings.drawer.gap, 0, 32);
    settings.drawer.cornerRadius = ParseInt(values, "drawer.corner_radius", settings.drawer.cornerRadius, 0, 32);
    settings.drawer.animationMs = ParseInt(values, "drawer.animation_ms", settings.drawer.animationMs, 0, 1000);
    settings.drawer.shortNameChars = ParseInt(values, "drawer.short_name_chars", settings.drawer.shortNameChars, 1, 16);
    settings.drawer.topOffset = ParseInt(values, "drawer.top_offset", settings.drawer.topOffset, 0, 800);
    settings.drawer.attachOverlap = ParseInt(values, "drawer.attach_overlap", settings.drawer.attachOverlap, 0, 24);
    settings.drawer.activeExtraExtent = ParseInt(values, "drawer.active_extra_extent", settings.drawer.activeExtraExtent, 0, 80);
    settings.drawer.bottomCollapsedExtent = ParseInt(values, "drawer.bottom_collapsed_extent", settings.drawer.bottomCollapsedExtent, 24, 240);
    settings.drawer.bottomExpandedExtent = ParseInt(values, "drawer.bottom_expanded_extent", settings.drawer.bottomExpandedExtent, settings.drawer.bottomCollapsedExtent, 480);
    settings.drawer.bottomCollapsedThickness = ParseInt(values, "drawer.bottom_collapsed_thickness", settings.drawer.bottomCollapsedThickness, 0, 80);
    settings.drawer.transparency = ParseInt(values, "drawer.transparency", settings.drawer.transparency, 0, 90);
    if (const auto it = values.find("drawer.active_window_only"); it != values.end()) {
        settings.drawer.activeWindowOnly = ParseBool(it->second, settings.drawer.activeWindowOnly);
    }

    if (const auto it = values.find("preview.enabled"); it != values.end()) {
        settings.preview.enabled = ParseBool(it->second, settings.preview.enabled);
    }
    settings.preview.delayMs = ParseInt(values, "preview.delay_ms", settings.preview.delayMs, 0, 5000);
    settings.preview.width = ParseInt(values, "preview.width", settings.preview.width, 160, 1600);
    settings.preview.height = ParseInt(values, "preview.height", settings.preview.height, 100, 1200);
    settings.preview.cornerRadius = ParseInt(values, "preview.corner_radius", settings.preview.cornerRadius, 0, 32);
    settings.performance.geometryThrottleMs = ParseInt(values, "performance.geometry_throttle_ms", settings.performance.geometryThrottleMs, 8, 250);

    if (const auto it = values.find("selection.disabled_apps"); it != values.end()) {
        settings.selection.disabledAppKeys = ParseEncodedList(it->second);
    }

    if (settings.drawer.expandedExtent < settings.drawer.collapsedExtent) {
        settings.drawer.expandedExtent = settings.drawer.collapsedExtent;
    }
    return settings;
}

bool Settings::Save(const std::filesystem::path& filePath, const Settings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(filePath.parent_path(), ec);

    std::ofstream output(filePath, std::ios::trunc);
    if (!output) {
        return false;
    }

    output << "# WindowMark v0.2.0\n";
    output << "# Edit this file, then restart WindowMark.\n\n";
    output << "placement=" << ToString(settings.drawer.placement) << "\n";
    output << "drawer.collapsed_extent=" << settings.drawer.collapsedExtent << "\n";
    output << "drawer.expanded_extent=" << settings.drawer.expandedExtent << "\n";
    output << "drawer.thickness=" << settings.drawer.thickness << "\n";
    output << "drawer.gap=" << settings.drawer.gap << "\n";
    output << "drawer.corner_radius=" << settings.drawer.cornerRadius << "\n";
    output << "drawer.animation_ms=" << settings.drawer.animationMs << "\n";
    output << "drawer.short_name_chars=" << settings.drawer.shortNameChars << "\n";
    output << "drawer.top_offset=" << settings.drawer.topOffset << "\n";
    output << "drawer.attach_overlap=" << settings.drawer.attachOverlap << "\n";
    output << "# Show bookmarks only on the foreground window. Turning this off restores a\n";
    output << "# strip on every window, but background strips can then overlap other windows.\n";
    output << "drawer.active_window_only=" << (settings.drawer.activeWindowOnly ? "true" : "false") << "\n";
    output << "drawer.active_extra_extent=" << settings.drawer.activeExtraExtent << "\n";
    output << "# Percent. 0 is fully opaque; raise it to let the window show through.\n";
    output << "drawer.transparency=" << settings.drawer.transparency << "\n\n";
    output << "# Bottom/top rows (used for maximized windows) size independently of the sides:\n";
    output << "# here the extent is a tab's width. A row tab rests at half thickness against\n";
    output << "# the window edge and grows upward on hover. 0 thickness means half of\n";
    output << "# drawer.thickness.\n";
    output << "drawer.bottom_collapsed_extent=" << settings.drawer.bottomCollapsedExtent << "\n";
    output << "drawer.bottom_expanded_extent=" << settings.drawer.bottomExpandedExtent << "\n";
    output << "drawer.bottom_collapsed_thickness=" << settings.drawer.bottomCollapsedThickness << "\n\n";
    output << "preview.enabled=" << (settings.preview.enabled ? "true" : "false") << "\n";
    output << "preview.delay_ms=" << settings.preview.delayMs << "\n";
    output << "preview.width=" << settings.preview.width << "\n";
    output << "preview.height=" << settings.preview.height << "\n";
    output << "preview.corner_radius=" << settings.preview.cornerRadius << "\n\n";
    output << "performance.geometry_throttle_ms=" << settings.performance.geometryThrottleMs << "\n\n";
    output << "# Application selections are persistent. Individual-window selections are session-only.\n";
    output << "selection.disabled_apps=" << EncodeList(settings.selection.disabledAppKeys) << "\n";
    return static_cast<bool>(output);
}

} // namespace windowmark
