using System.Windows;
using System.Windows.Controls;

namespace Skatestrap;

public partial class PerformancePage : Page
{
    bool ready;

    public PerformancePage()
    {
        InitializeComponent();
        PerfSwitch.IsChecked = Settings.Current.PerfTweaks;
        GraphicsPage.Pick(PriorityBox, Settings.Current.Priority);
        ready = true;
    }

    void Changed(object sender, RoutedEventArgs e)
    {
        if (!ready) return;
        Settings.Current.PerfTweaks = PerfSwitch.IsChecked == true;
        Settings.Current.Priority = GraphicsPage.Tag(PriorityBox);
    }
}
