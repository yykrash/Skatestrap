using Wpf.Ui.Controls;

namespace Skatestrap;

public partial class MainWindow : FluentWindow
{
    public MainWindow()
    {
        InitializeComponent();
        Loaded += async (_, _) =>
        {
            UpdateOverlay.Visibility = System.Windows.Visibility.Visible;
            try { System.IO.File.Delete(System.IO.Path.Combine(Settings.Root, "Skatestrap.exe.old")); } catch { }  // left by the last self-update
            if (await Updater.CheckAndApply(msg => Dispatcher.Invoke(() =>
            {
                if (msg == "") UpdateOverlay.Visibility = System.Windows.Visibility.Collapsed;
                else UpdateText.Text = msg;
            }))) { System.Windows.Application.Current.Shutdown(); return; }  // the new Skatestrap.exe is starting
            UpdateOverlay.Visibility = System.Windows.Visibility.Collapsed;
            Nav.Navigate(typeof(PlayPage));
        };
        Closing += (_, _) => Settings.Current.Save();
    }
}
