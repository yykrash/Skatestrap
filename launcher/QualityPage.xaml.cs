using System.Windows;
using System.Windows.Controls;

namespace Skatestrap;

public partial class QualityPage : Page
{
    bool ready;

    public QualityPage()
    {
        InitializeComponent();
        var s = Settings.Current;
        LowSwitch.IsChecked = s.LowGraphics; GrassSwitch.IsChecked = s.NoGrass; StillSwitch.IsChecked = s.StillGrass;
        GraphicsPage.Pick(MsaaBox, s.Msaa); GraphicsPage.Pick(TexBox, s.Textures);
        ready = true;
    }

    void Changed(object sender, RoutedEventArgs e)
    {
        if (!ready) return;
        var s = Settings.Current;
        s.LowGraphics = LowSwitch.IsChecked == true; s.NoGrass = GrassSwitch.IsChecked == true; s.StillGrass = StillSwitch.IsChecked == true;
        s.Msaa = GraphicsPage.Tag(MsaaBox); s.Textures = GraphicsPage.Tag(TexBox);
    }
}
