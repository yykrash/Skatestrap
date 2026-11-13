using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;

namespace Skatestrap;

public partial class ModsPage : Page
{
    public ModsPage()
    {
        InitializeComponent();
        ModsSwitch.IsChecked = Settings.Current.Mods;
        PsSwitch.IsChecked = Settings.Current.PsIcons;
        PcSwitch.IsChecked = Settings.Current.PcMode;
    }

    void Changed(object sender, RoutedEventArgs e)
    {
        Settings.Current.Mods = ModsSwitch.IsChecked == true;
        Settings.Current.PsIcons = PsSwitch.IsChecked == true;
        Settings.Current.PcMode = PcSwitch.IsChecked == true;
    }

    void Open_Click(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(Settings.ModsDir);
        Process.Start(new ProcessStartInfo(Settings.ModsDir) { UseShellExecute = true });
    }
}
