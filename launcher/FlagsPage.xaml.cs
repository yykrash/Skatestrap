using System.Collections.ObjectModel;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;

namespace Skatestrap;

public partial class FlagsPage : Page
{
    public class Row { public string Name { get; set; } = ""; public string Value { get; set; } = ""; }
    readonly ObservableCollection<Row> rows = new();

    public FlagsPage()
    {
        InitializeComponent();
        foreach (var (k, v) in Settings.Current.Flags.OrderBy(p => p.Key)) rows.Add(new Row { Name = k, Value = v });
        Grid.ItemsSource = rows;
    }

    // the grid is the truth; settings are rebuilt from it after every change (empty names are dropped)
    void Store()
    {
        var f = Settings.Current.Flags;
        f.Clear();
        foreach (var r in rows) if (r.Name.Trim() != "") f[r.Name.Trim()] = r.Value.Trim();
    }

    void Grid_CellEditEnding(object? sender, DataGridCellEditEndingEventArgs e) =>
        Dispatcher.BeginInvoke(Store);  // after the edit is written into the row

    void Add_Click(object sender, RoutedEventArgs e)
    {
        var r = new Row { Name = "FFlag", Value = "True" };
        rows.Add(r);
        Grid.SelectedItem = r;
        Grid.ScrollIntoView(r);
    }

    void Remove_Click(object sender, RoutedEventArgs e)
    {
        foreach (var r in Grid.SelectedItems.Cast<Row>().ToList()) rows.Remove(r);
        Store();
    }

    void Import_Click(object sender, RoutedEventArgs e)
    {
        var box = new System.Windows.Controls.TextBox { AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, Height = 260, Width = 460,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        var dialog = new Wpf.Ui.Controls.MessageBox
        {
            Title = "Import flags", Content = box, PrimaryButtonText = "Import", CloseButtonText = "Cancel",
        };
        dialog.ShowDialogAsync().ContinueWith(t =>
        {
            if (t.Result != Wpf.Ui.Controls.MessageBoxResult.Primary) return;
            try
            {
                using var doc = JsonDocument.Parse(box.Text);
                foreach (var p in doc.RootElement.EnumerateObject())
                {
                    var value = p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString()! : p.Value.GetRawText();
                    var old = rows.FirstOrDefault(r => r.Name == p.Name);
                    if (old != null) rows.Remove(old);
                    rows.Add(new Row { Name = p.Name, Value = value });
                }
                Store();
            }
            catch (JsonException ex) { System.Windows.MessageBox.Show("That is not a JSON object of flags:\n" + ex.Message, "Skatestrap"); }
        }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    void Export_Click(object sender, RoutedEventArgs e) =>
        Clipboard.SetText(JsonSerializer.Serialize(Settings.Current.Flags, new JsonSerializerOptions { WriteIndented = true }));
}
