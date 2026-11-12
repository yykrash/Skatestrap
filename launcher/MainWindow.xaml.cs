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
            await Updater.CheckAndApply(msg => Dispatcher.Invoke(() =>
            {
                if (msg == "") UpdateOverlay.Visibility = System.Windows.Visibility.Collapsed;
                else UpdateText.Text = msg;
            }));
            UpdateOverlay.Visibility = System.Windows.Visibility.Collapsed;
            Nav.Navigate(typeof(PlayPage));
        };
        Closing += (_, _) => Settings.Current.Save();
    }
}
