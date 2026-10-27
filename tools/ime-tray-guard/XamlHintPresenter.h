#pragma once
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Text.h>
#include <algorithm>
#include <vector>

namespace container_trial {
// 提示语是保留区中的普通 XAML 文字，不注册托盘图标，不接收点击。
class HintPresenter {
    using Element=winrt::Windows::UI::Xaml::FrameworkElement;
    using Grid=winrt::Windows::UI::Xaml::Controls::Grid;
    using Text=winrt::Windows::UI::Xaml::Controls::TextBlock;
    Element target_{nullptr};
    Grid grid_{nullptr};
    Text text_{nullptr};
    winrt::event_token layoutToken_{};
    bool subscribed_{}, enabled_{}, fixture_{}, updating_{};
    unsigned phrase_{~0U};
    double left_{}, width_{};
public:
    ~HintPresenter(){Clear();}
    bool Visible() const { return text_ && text_.Visibility()==winrt::Windows::UI::Xaml::Visibility::Visible; }
    double Left() const { return left_; }
    double Width() const { return width_; }
    void Clear() noexcept {
        try {
            if(subscribed_)target_.LayoutUpdated(layoutToken_);
            if(grid_ && text_) {
                uint32_t index{};
                if(grid_.Children().IndexOf(text_,index))grid_.Children().RemoveAt(index);
            }
        } catch(...) {}
        subscribed_=false;enabled_=false;target_=nullptr;grid_=nullptr;text_=nullptr;
        phrase_=~0U;left_=width_=0;
    }
    void Update(Element const& target,bool enabled,bool fixture) {
        using namespace winrt::Windows::UI::Xaml;
        if(!enabled){Clear();return;}
        if(target_!=target) {
            Clear();target_=target;fixture_=fixture;
            if(fixture)grid_=target.try_as<Grid>();
            else for(int i=0;i<Media::VisualTreeHelper::GetChildrenCount(target);++i) {
                auto child=Media::VisualTreeHelper::GetChild(target,i).try_as<Grid>();
                if(child && child.Name()==L"Content"){grid_=child;break;}
            }
            if(!grid_){Clear();return;}
            text_=Text();text_.Name(L"WindowMarkImeHintLabel");
            text_.IsHitTestVisible(false);text_.FontSize(12);
            text_.HorizontalAlignment(HorizontalAlignment::Left);
            text_.VerticalAlignment(VerticalAlignment::Center);
            text_.TextAlignment(TextAlignment::Center);
            text_.TextTrimming(TextTrimming::CharacterEllipsis);
            text_.Visibility(Visibility::Collapsed);
            grid_.Children().Append(text_);
            layoutToken_=target_.LayoutUpdated([this](auto const&,auto const&){Refresh();});
            subscribed_=true;
        }
        enabled_=true;Refresh();
    }
private:
    void Refresh() noexcept {
        using namespace winrt::Windows::UI::Xaml;
        if(!enabled_ || !text_ || updating_)return;
        updating_=true;
        try {
            struct Pending {DependencyObject object;bool visible;};
            std::vector<Pending> pending{{target_,true}};
            unsigned visited{};double occupiedRight{};bool language{};
            while(!pending.empty() && ++visited<=600) {
                auto p=std::move(pending.back());pending.pop_back();
                auto fe=p.object.try_as<Element>();
                if(fe==text_)continue;
                bool visible=p.visible && (!fe || (fe.Visibility()==Visibility::Visible && fe.ActualWidth()>0));
                auto type=winrt::get_class_name(p.object);
                if(visible && fe && (type==L"SystemTray.IconView" ||
                    (fixture_ && fe.Name()==L"WindowMarkFixtureIcon"))) {
                    auto at=fe.TransformToVisual(target_).TransformPoint({0,0});
                    occupiedRight=std::max(occupiedRight,static_cast<double>(at.X)+fe.ActualWidth());
                }
                if(visible && (type==L"SystemTray.LanguageImageIconContent" || type==L"SystemTray.LanguageTextIconContent"))language=true;
                const int children=Media::VisualTreeHelper::GetChildrenCount(p.object);
                for(int i=0;i<children;++i)pending.push_back({Media::VisualTreeHelper::GetChild(p.object,i),visible});
            }
            const double right=target_.ActualWidth();
            const double left=std::clamp(occupiedRight,0.0,right);
            const double available=right-left;
            // 只有宽度事务已在原生输入法可见时完成学习，调用方才启用本对象。
            // 不在文字开关处重新学习图标数，否则隐藏期间重新打开文字会一直空白。
            const bool show=visited<=600 && pending.empty() && !language && available>=48;
            if(show) {
                if(left_!=left || width_!=available) {
                    left_=left;width_=available;
                    text_.Margin({left,0,0,0});text_.Width(available);
                }
                constexpr const wchar_t* phrases[]={L"加油鸭",L"喝杯水",L"歇一会",L"眨眨眼",L"慢慢来",L"伸个懒腰"};
                const unsigned index=static_cast<unsigned>((GetTickCount64()/45000)%std::size(phrases));
                if(phrase_!=index){text_.Text(phrases[index]);phrase_=index;}
                const bool dark=target_.ActualTheme()==ElementTheme::Dark;
                const BYTE shade=dark?215:85;
                auto brush=text_.Foreground().try_as<Media::SolidColorBrush>();
                if(!brush || brush.Color().R!=shade)text_.Foreground(Media::SolidColorBrush({255,shade,shade,shade}));
            }
            const auto visibility=show?Visibility::Visible:Visibility::Collapsed;
            if(text_.Visibility()!=visibility)text_.Visibility(visibility);
        } catch(...) {try{text_.Visibility(Visibility::Collapsed);}catch(...) {}}
        updating_=false;
    }
};
}
