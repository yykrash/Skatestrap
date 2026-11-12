using System.Windows;
using System.Windows.Controls;

namespace Skatestrap;

public partial class DebugPage : Page
{
    public DebugPage()
    {
        InitializeComponent();
        var s = Settings.Current;
        FpsSwitch.IsChecked = s.FpsStats;
        ProfSwitch.IsChecked = s.Profiler;
        DumpSwitch.IsChecked = s.FreezeDump;
        SendSwitch.IsChecked = s.SendLog;
    }

    void Changed(object sender, RoutedEventArgs e)
    {
        var s = Settings.Current;
        s.FpsStats = FpsSwitch.IsChecked == true;
        s.Profiler = ProfSwitch.IsChecked == true;
        s.FreezeDump = DumpSwitch.IsChecked == true;
        s.SendLog = SendSwitch.IsChecked == true;
    }

    void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        if (System.IO.File.Exists(Launcher.LogPath))
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Launcher.LogPath) { UseShellExecute = true });
    }
}
