using System.Windows;
using System.Windows.Controls;

namespace Skatestrap;

public partial class GraphicsPage : Page
{
    bool ready;

    public GraphicsPage()
    {
        InitializeComponent();
        var s = Settings.Current;
        RendererBox.SelectedIndex = s.Renderer == "angle" ? 1 : s.Renderer == "native" ? 2 : 0;
        var preset = FpsBox.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == s.FpsCap);
        if (preset != null) FpsBox.SelectedItem = preset; else FpsBox.Text = s.FpsCap;
        Pick(ScaleBox, s.RenderScale);
        FsrSwitch.IsChecked = s.Fsr;
        FullSwitch.IsChecked = s.Fullscreen;
        ready = true;
    }

    void Renderer_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (ready) Settings.Current.Renderer = (string)((ComboBoxItem)RendererBox.SelectedItem).Tag;
    }

    void Fps_Changed(object sender, RoutedEventArgs e)
    {
        if (!ready) return;
        if (FpsBox.SelectedItem is ComboBoxItem item) { Settings.Current.FpsCap = (string)item.Tag; return; }
        var typed = FpsBox.Text.Trim();
        // the loader takes a positive number or "unlimited"; anything else means Roblox's default
        Settings.Current.FpsCap = int.TryParse(typed, out var n) && n > 0 ? n.ToString()
            : typed.Equals("unlimited", StringComparison.OrdinalIgnoreCase) ? "unlimited" : "";
    }

    void Fsr_Click(object sender, RoutedEventArgs e) => Settings.Current.Fsr = FsrSwitch.IsChecked == true;
    void Full_Click(object sender, RoutedEventArgs e) => Settings.Current.Fullscreen = FullSwitch.IsChecked == true;

    // select the item whose Tag matches (first item if none); shared by the settings pages
    internal static void Pick(ComboBox box, string tag) =>
        box.SelectedItem = box.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == tag) ?? box.Items[0];
    internal static string Tag(ComboBox box) => (string)((ComboBoxItem)box.SelectedItem).Tag;

    void Scale_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (ready) Settings.Current.RenderScale = Tag(ScaleBox);
    }
}
