#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace windowmark::win {

// 浮动标题：一块浅色圆角底，上面一层或几层文字。主标签换人时新旧两层同时存在，一个淡出一个
// 淡入，所以是「几层」。
//
// 横排书签用横排文字。左右两侧用真正的竖排排版（DirectWrite 的 TOP_TO_BOTTOM 阅读方向）：中文
// 字形直立，拉丁字母按竖排规则侧转，两侧都从上往下读。
//
// 量和画放在同一个类里：量出来多长，画的时候就按同一个字体、同一套内边距排，两边不会对不上。
// 只依赖 render target，不认识窗口——书签条的预览端画到分层窗口上，检查工具画到图片上。
class PreviewTitle {
public:
    struct Layer {
        std::wstring text;
        float opacity{};
    };

    explicit PreviewTitle(IDWriteFactory* factory) : factory_(factory) {}

    // 按显示器缩放设字号和内边距。变了才会清缓存。
    void SetScale(float scale);

    // 沿阅读方向的自然长度，含两端内边距：横排是宽，竖排是高。
    [[nodiscard]] float NaturalLength(const std::wstring& text);
    // 垂直于阅读方向的厚度，含内边距：横排是高，竖排是宽。
    [[nodiscard]] float Thickness();

    // 在 rect 里画圆角底和各层文字。文字放不下时用省略号收尾，不换行。
    void Draw(ID2D1RenderTarget& target, const D2D1_RECT_F& rect, bool vertical,
              const std::vector<Layer>& layers);

private:
    [[nodiscard]] float FontPx() const { return 13.0F * scale_; }
    [[nodiscard]] float PadMain() const { return 10.0F * scale_; }
    [[nodiscard]] float PadCross() const { return 5.0F * scale_; }
    IDWriteTextFormat* Format(bool vertical);
    IDWriteTextLayout* Layout(const std::wstring& text, bool vertical);

    IDWriteFactory* factory_{};
    float scale_{1.0F};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> horizontal_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> vertical_;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsisH_;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsisV_;
    std::map<std::pair<std::wstring, bool>, Microsoft::WRL::ComPtr<IDWriteTextLayout>> layouts_;
    std::map<std::wstring, float> lengths_;
    float thickness_{};
};

} // namespace windowmark::win
