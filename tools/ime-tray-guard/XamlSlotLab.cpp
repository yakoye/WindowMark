// 隔离实验：仅在自身进程创建 XAML 元素，不创建可见窗口、不附加 Explorer。
#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include "XamlWidthOverride.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {
int checks{};
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
    std::cout << "PASS " << message << std::endl;
}
double Width(FrameworkElement const& element) {
    element.InvalidateMeasure();
    element.Measure({1000.0F, 100.0F});
    const auto size = element.DesiredSize();
    element.Arrange({0.0F, 0.0F, size.Width, size.Height});
    return size.Width;
}
bool Near(double actual, double expected) { return std::abs(actual - expected) < 0.01; }
Border Content(double width) {
    Border element;
    element.Width(width);
    element.Height(48);
    return element;
}
void Test() {
    // 125% DPI 下采到 40+55px；此处仅用对应的 32+44 DIP 验证布局，不硬编码产品参数。
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    Grid slotA, slotB;
    auto iconA = Content(32), iconB = Content(44);
    slotA.Children().Append(iconA);
    slotB.Children().Append(iconB);
    row.Children().Append(slotA);
    row.Children().Append(slotB);
    Require(Near(Width(row),76), "visible content requests 76 DIP");

    const auto oldMinA = slotA.ReadLocalValue(FrameworkElement::MinWidthProperty());
    const auto oldMinB = slotB.ReadLocalValue(FrameworkElement::MinWidthProperty());
    Require(oldMinA == DependencyProperty::UnsetValue() && oldMinB == DependencyProperty::UnsetValue(),
            "parent widths initially have no local override");
    iconA.Visibility(Visibility::Collapsed);
    iconB.Visibility(Visibility::Collapsed);
    Require(Near(Width(row),0), "collapsed content does not keep explicit Width");

    // 修改的只有仍参与布局的父容器；内容继续遵从原始显隐，不改成假字/假图标。
    slotA.MinWidth(32);
    slotB.MinWidth(44);
    slotA.IsHitTestVisible(false);
    slotB.IsHitTestVisible(false);
    Require(Near(Width(row),76), "visible stable parents retain empty space");
    Require(!slotA.IsHitTestVisible() && !slotB.IsHitTestVisible(), "empty slots disable hit testing property");
    Require(!slotA.Background() && !slotB.Background(), "empty slots have no background brush");

    iconA.Visibility(Visibility::Visible);
    slotA.IsHitTestVisible(true);
    Require(Near(Width(row),76), "one original content visible keeps total width");
    Require(slotA.Children().GetAt(0) == iconA, "original content instance is retained");

    // 宿主自身被折叠时，MinWidth 不能解决问题。必须在真实树里找到稳定层。
    slotA.Visibility(Visibility::Collapsed);
    slotB.Visibility(Visibility::Collapsed);
    Require(Near(Width(row),0), "collapsed parent defeats reserved MinWidth");
    slotA.Visibility(Visibility::Visible);
    slotB.Visibility(Visibility::Visible);

    slotA.Children().Clear();
    slotB.Children().Clear();
    Require(Near(Width(row),76), "removed children still leave stable parent slots");
    slotA.Children().Append(iconA);
    slotB.Children().Append(iconB);
    iconB.Visibility(Visibility::Visible);
    Require(Near(Width(row),76), "restored children keep width");

    slotA.ClearValue(FrameworkElement::MinWidthProperty());
    slotB.ClearValue(FrameworkElement::MinWidthProperty());
    slotA.ClearValue(UIElement::IsHitTestVisibleProperty());
    slotB.ClearValue(UIElement::IsHitTestVisibleProperty());
    Require(slotA.ReadLocalValue(FrameworkElement::MinWidthProperty()) == oldMinA &&
            slotB.ReadLocalValue(FrameworkElement::MinWidthProperty()) == oldMinB,
            "disable restores unset property state rather than setting a guessed zero");
    iconA.Visibility(Visibility::Collapsed);
    iconB.Visibility(Visibility::Collapsed);
    Require(Near(Width(row),0), "disable returns original collapsing layout");
    Require(slotA.IsHitTestVisible() && slotB.IsHitTestVisible(), "disable restores default hit testing property");

    Grid explicitParent;
    explicitParent.MinWidth(13);
    const auto oldExplicit = explicitParent.ReadLocalValue(FrameworkElement::MinWidthProperty());
    explicitParent.MinWidth(32);
    explicitParent.SetValue(FrameworkElement::MinWidthProperty(),oldExplicit);
    Require(Near(explicitParent.MinWidth(),13), "restore nonzero original local value");

    Grid boundParent;
    Data::Binding originalBinding;
    originalBinding.Source(winrt::box_value(23.0));
    Data::BindingOperations::SetBinding(boundParent,FrameworkElement::MinWidthProperty(),originalBinding);
    const auto expression = boundParent.GetBindingExpression(FrameworkElement::MinWidthProperty());
    Require(expression && Near(boundParent.MinWidth(),23), "binding is active before override");
    const auto savedBinding = expression.ParentBinding();
    boundParent.MinWidth(76);
    Require(Near(boundParent.MinWidth(),76), "temporary override changes effective bound value");
    Data::BindingOperations::SetBinding(boundParent,FrameworkElement::MinWidthProperty(),savedBinding);
    Require(boundParent.GetBindingExpression(FrameworkElement::MinWidthProperty()) && Near(boundParent.MinWidth(),23),
            "restore original binding, not only its old numeric result");

    container_trial::WidthOverride transaction;
    Grid target;
    transaction.Apply(target,76);
    Require(transaction.Active() && Near(target.MinWidth(),76), "shared transaction applies measured width");
    Require(transaction.Restore() && target.ReadLocalValue(FrameworkElement::MinWidthProperty())==DependencyProperty::UnsetValue(), "shared transaction restores unset");
    Require(transaction.Restore(), "shared transaction repeated restore is idempotent");
    target.MinWidth(17);
    transaction.Apply(target,76);
    Require(transaction.Restore() && Near(target.MinWidth(),17), "shared transaction restores nonzero local value");
    transaction.Apply(boundParent,76);
    Require(transaction.Restore() && boundParent.GetBindingExpression(FrameworkElement::MinWidthProperty()).ParentBinding()==savedBinding && Near(boundParent.MinWidth(),23), "shared transaction restores original binding object");
    bool rejected=false; target.Visibility(Visibility::Collapsed);
    try { transaction.Apply(target,76); } catch(std::exception const&) {rejected=true;}
    Require(rejected && !transaction.Active(), "shared transaction rejects collapsed target before writing");
    target.Visibility(Visibility::Visible);
    for(double invalid:{0.0,-1.0,401.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        rejected=false;
        try {transaction.Apply(target,invalid);}catch(std::exception const&){rejected=true;}
        Require(rejected && !transaction.Active() && Near(target.MinWidth(),17), "shared transaction rejects invalid width without change");
    }
}
} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        auto manager = winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
        Test();
        manager.Close();
        std::cout << "Isolated XAML checks: " << checks << "; Explorer modified: false; native guard validated: false" << std::endl;
        return 0;
    } catch (winrt::hresult_error const& error) {
        std::cerr << "XAML HRESULT: 0x" << std::hex << static_cast<unsigned long>(error.code().value)
                  << " " << winrt::to_string(error.message()) << std::endl;
        return 2;
    } catch (std::exception const& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
