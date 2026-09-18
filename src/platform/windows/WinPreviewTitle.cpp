#include "WinPreviewTitle.h"

#include <algorithm>
#include <cmath>

namespace windowmark::win {

void PreviewTitle::SetScale(float scale) {
    if (std::fabs(scale - scale_) < 0.001F) return;
    scale_ = scale;
    horizontal_.Reset();
    vertical_.Reset();
    ellipsisH_.Reset();
    ellipsisV_.Reset();
    layouts_.clear();
    lengths_.clear();
    thickness_ = 0.0F;
}

IDWriteTextFormat* PreviewTitle::Format(bool vertical) {
    auto& slot = vertical ? vertical_ : horizontal_;
    if (slot) return slot.Get();
    if (!factory_) return nullptr;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    HRESULT hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        FontPx(), L"zh-CN", &format);
    if (FAILED(hr)) {
        hr = factory_->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            FontPx(), L"zh-CN", &format);
        if (FAILED(hr)) return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    // 两个方向都居中：沿阅读方向居中（短标题在底的正中），垂直于阅读方向也居中。
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (vertical) {
        // 一行从上往下写；行与行从右往左排。只有一行，后者不影响结果，但两个方向必须互相
        // 垂直，这个组合才合法。
        format->SetReadingDirection(DWRITE_READING_DIRECTION_TOP_TO_BOTTOM);
        format->SetFlowDirection(DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT);
    }

    // 放不下就在末尾放省略号，按字符截，不换行——换行会把三段式的结构撑坏。
    auto& ellipsis = vertical ? ellipsisV_ : ellipsisH_;
    if (SUCCEEDED(factory_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        format->SetTrimming(&trimming, ellipsis.Get());
    }

    slot = std::move(format);
    return slot.Get();
}

IDWriteTextLayout* PreviewTitle::Layout(const std::wstring& text, bool vertical) {
    const auto key = std::make_pair(text, vertical);
    if (const auto it = layouts_.find(key); it != layouts_.end()) return it->second.Get();

    IDWriteTextFormat* format = Format(vertical);
    if (!format) return nullptr;
    // 标题只在鼠标扫过的那几个书签之间换，缓存一小撮就够，满了整个清掉。
    if (layouts_.size() >= 16) layouts_.clear();

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                          100.0F, 100.0F, &layout))) {
        return nullptr;
    }
    auto [slot, inserted] = layouts_.emplace(key, std::move(layout));
    (void)inserted;
    return slot->second.Get();
}

float PreviewTitle::NaturalLength(const std::wstring& text) {
    if (const auto it = lengths_.find(text); it != lengths_.end()) return it->second;

    // 横排量出来的宽度也就是竖排的长度：中文直立时每个字的竖向步进和横向一样是一个字宽，
    // 拉丁字母竖排是整体侧转，步进不变。
    float width = 0.0F;
    IDWriteTextFormat* format = Format(false);
    if (format && !text.empty()) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                 format, 100000.0F, 1000.0F, &layout))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                width = metrics.widthIncludingTrailingWhitespace;
            }
        }
    }
    // 多留 1px：量出来的宽度是小数，差一丝就会被当成放不下，末尾平白多出一个省略号。
    const float length = std::ceil(width) + 1.0F + 2.0F * PadMain();
    if (lengths_.size() >= 64) lengths_.clear();
    lengths_.emplace(text, length);
    return length;
}

float PreviewTitle::Thickness() {
    if (thickness_ > 0.0F) return thickness_;
    // 行高要按中文量：拉丁字母的行高比中文矮一截，只量字母会让中文顶到底的边上。
    float line = FontPx() * 1.4F;
    if (IDWriteTextFormat* format = Format(false)) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const wchar_t sample[] = L"国Ag";
        if (SUCCEEDED(factory_->CreateTextLayout(sample, 3, format, 1000.0F, 1000.0F,
                                                 &layout))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.height > 0.0F) {
                line = metrics.height;
            }
        }
    }
    thickness_ = std::ceil(line) + 2.0F * PadCross();
    return thickness_;
}

void PreviewTitle::Draw(ID2D1RenderTarget& target, const D2D1_RECT_F& rect, bool vertical,
                        const std::vector<Layer>& layers) {
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 1.0F || h <= 1.0F) return;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target.CreateSolidColorBrush(D2D1::ColorF(0.0F, 0.0F, 0.0F, 1.0F), &brush))) {
        return;
    }

    // 浅色底：标题压在任意窗口内容上都得读得清。
    const float radius = std::min(6.0F * scale_, std::min(w, h) * 0.5F);
    const D2D1_ROUNDED_RECT body = D2D1::RoundedRect(
        D2D1::RectF(rect.left + 0.5F, rect.top + 0.5F, rect.right - 0.5F, rect.bottom - 0.5F),
        radius, radius);
    brush->SetColor(D2D1::ColorF(0.973F, 0.976F, 0.984F, 1.0F));
    target.FillRoundedRectangle(body, brush.Get());
    brush->SetColor(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.16F));
    target.DrawRoundedRectangle(body, brush.Get(), 1.0F);

    const float padX = vertical ? PadCross() : PadMain();
    const float padY = vertical ? PadMain() : PadCross();
    const float boxW = std::max(1.0F, w - 2.0F * padX);
    const float boxH = std::max(1.0F, h - 2.0F * padY);
    for (const Layer& layer : layers) {
        if (layer.opacity <= 0.0F || layer.text.empty()) continue;
        IDWriteTextLayout* layout = Layout(layer.text, vertical);
        if (!layout) continue;
        layout->SetMaxWidth(boxW);
        layout->SetMaxHeight(boxH);
        brush->SetColor(
            D2D1::ColorF(0.09F, 0.10F, 0.12F, 0.94F * std::clamp(layer.opacity, 0.0F, 1.0F)));
        target.DrawTextLayout(D2D1::Point2F(rect.left + padX, rect.top + padY), layout,
                              brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

} // namespace windowmark::win
