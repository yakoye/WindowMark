#pragma once
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <cmath>
#include <stdexcept>

namespace container_trial {
// 仅在所属UI线程使用；原有对象与绑定保留到恢复验证完成。
class WidthOverride {
    winrt::Windows::UI::Xaml::FrameworkElement element_{nullptr};
    winrt::Windows::Foundation::IInspectable old_{nullptr};
    winrt::Windows::UI::Xaml::Data::BindingBase binding_{nullptr};
    bool dirty_{};
public:
    bool Active() const { return dirty_; }
    void Apply(winrt::Windows::UI::Xaml::FrameworkElement const& element, double width) {
        using namespace winrt::Windows::UI::Xaml;
        if (dirty_ || !std::isfinite(width) || width<=0 || width>400) throw std::runtime_error("Unsafe width override");
        if (element.Visibility()!=Visibility::Visible) throw std::runtime_error("Collapsed host is not a reservation target");
        auto property=FrameworkElement::MinWidthProperty();
        auto expression=element.GetBindingExpression(property);
        auto local=element.ReadLocalValue(property);
        // 不把未知表达式当作普通数字覆盖。
        if (!expression && local!=DependencyProperty::UnsetValue() && !local.try_as<winrt::Windows::Foundation::IPropertyValue>())
            throw std::runtime_error("Unknown local value");
        element_=element; old_=local; binding_=expression?expression.ParentBinding():nullptr;
        dirty_=true; // 写入抛异常也必须允许恢复。
        element.MinWidth(width);
        if (std::abs(element.MinWidth()-width)>0.01) throw std::runtime_error("Width readback mismatch");
    }
    bool Restore() noexcept {
        if (!dirty_) return true;
        try {
            using namespace winrt::Windows::UI::Xaml;
            auto property=FrameworkElement::MinWidthProperty();
            if (binding_) Data::BindingOperations::SetBinding(element_,property,binding_);
            else if (old_==DependencyProperty::UnsetValue()) element_.ClearValue(property);
            else element_.SetValue(property,old_);
            bool ok=false;
            if (binding_) {
                auto restored=element_.GetBindingExpression(property);
                ok=restored && restored.ParentBinding()==binding_;
            } else if (old_==DependencyProperty::UnsetValue()) ok=element_.ReadLocalValue(property)==old_;
            else ok=std::abs(element_.MinWidth()-winrt::unbox_value<double>(old_))<0.01;
            if (!ok) return false;
            dirty_=false; element_=nullptr; old_=nullptr; binding_=nullptr; return true;
        } catch (...) { return false; }
    }
};
}
