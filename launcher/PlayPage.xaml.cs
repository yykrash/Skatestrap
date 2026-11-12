using System.Net.Http;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;

namespace Skatestrap;

public partial class PlayPage : Page
{
    readonly System.Windows.Threading.DispatcherTimer tick = new() { Interval = TimeSpan.FromMilliseconds(500) };
    bool wasRunning;

    // one "Continue playing" card
    public record Card(long PlaceId, string Name, string Creator, string Stats, string Joined, string? Image);

    public PlayPage()
    {
        InitializeComponent();
        if (Updater.LastError != null) StatusText.Text = "Update check failed: " + Updater.LastError;
        wasRunning = Launcher.Running != null;
        Running(wasRunning);
        // the game's exit is seen by polling: Launcher.Running goes back to null
        tick.Tick += (_, _) =>
        {
            var on = Launcher.Running != null;
            if (on == wasRunning) return;
            wasRunning = on; Running(on);
            if (!on) { LearnWho(); _ = LoadRecent(); }
        };
        Loaded += (_, _) => { tick.Start(); _ = LoadRecent(); FillAccounts(); LearnWho(); };
        Unloaded += (_, _) => tick.Stop();
    }

    // sign-in happens on Roblox's own screen and lands in login.bin; look up who it is each time the game closes
    // (the player may have switched account in the app), and keep it in the account list
    async void LearnWho()
    {
        var s = Settings.Current;
        var c = Login.Has ? Login.Cookie() : null;
        if (c == null) { s.UserId = s.Username = ""; s.Save(); FillAccounts(); return; }
        try
        {
            var who = await Launcher.WhoIs(c);
            if (who == null) return;
            (s.UserId, s.Username) = who.Value;
            Login.Keep(s.UserId); s.Accounts[s.UserId] = s.Username; s.Save();
            FillAccounts();
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or System.IO.IOException or ArgumentException) { }
    }

    // the account picker: saved accounts (signed-in one selected) + "Add account"
    bool filling;
    void FillAccounts()
    {
        var s = Settings.Current;
        filling = true;
        AccountBox.Items.Clear();
        foreach (var id in Login.Accounts())
        {
            var item = new ComboBoxItem { Content = s.Accounts.GetValueOrDefault(id, id), Tag = id };
            AccountBox.Items.Add(item);
            if (id == s.UserId && Login.Has) AccountBox.SelectedItem = item;
        }
        AccountBox.Items.Add(new ComboBoxItem { Content = "+ Add account" });
        RemoveButton.IsEnabled = AccountBox.SelectedItem != null && Launcher.Running == null;
        filling = false;
    }

    void Account_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (filling || AccountBox.SelectedItem is not ComboBoxItem item) return;
        var s = Settings.Current;
        try
        {
            if (item.Tag is string id) { Login.Use(id); s.UserId = id; s.Username = s.Accounts.GetValueOrDefault(id, ""); s.Save(); }
            else if (Login.Show(Window.GetWindow(this)) is { } who)  // roblox.com sign-in window; writes login.bin
            {
                (s.UserId, s.Username) = who;
                Login.Keep(who.id); s.Accounts[who.id] = who.name; s.Save();
            }
        }
        catch (Exception ex) when (ex is System.IO.IOException or ArgumentException) { StatusText.Text = "Could not switch account: " + ex.Message; }
        FillAccounts();
    }

    void Remove_Click(object sender, RoutedEventArgs e)
    {
        if (AccountBox.SelectedItem is not ComboBoxItem { Tag: string id }) return;
        var s = Settings.Current;
        if (MessageBox.Show(Window.GetWindow(this), $"Remove {s.Accounts.GetValueOrDefault(id, id)} from Skatestrap? You can add it again later.", "Remove account", MessageBoxButton.YesNo) != MessageBoxResult.Yes) return;
        Login.Drop(id); s.Accounts.Remove(id);
        if (s.UserId == id) { Login.Forget(); s.UserId = s.Username = ""; }  // next Play shows Roblox's sign-in
        s.Save();
        FillAccounts();
    }

    // the launcher steps aside while the game runs (minimized) and comes back when it closes
    void Running(bool on)
    {
        PlayButton.IsEnabled = !on; StopButton.IsEnabled = on; AccountBox.IsEnabled = !on;
        RemoveButton.IsEnabled = !on && AccountBox.SelectedItem is ComboBoxItem { Tag: string };
        if (Window.GetWindow(this) is not { } w) return;  // (null while the page is being built)
        if (on) w.WindowState = WindowState.Minimized;
        else if (w.WindowState == WindowState.Minimized) { w.WindowState = WindowState.Normal; w.Activate(); }
    }

    // name, maker, likes, players and picture from Roblox's public game APIs (no login sent)
    async Task LoadRecent()
    {
        var s = Settings.Current;
        List<RecentGame> list;
        lock (s.Recent) list = s.Recent.ToList();
        RecentTitle.Visibility = list.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (list.Count == 0) { RecentList.ItemsSource = null; return; }
        var info = new Dictionary<long, (string name, string creator, long playing)>();
        var votes = new Dictionary<long, string>();
        var pics = new Dictionary<long, string>();
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
            async Task<JsonElement> Get(string url) => JsonDocument.Parse(await http.GetStringAsync(url)).RootElement;
            foreach (var r in list.Where(r => r.UniverseId == 0))
                r.UniverseId = (await Get($"https://apis.roblox.com/universes/v1/places/{r.PlaceId}/universe")).GetProperty("universeId").GetInt64();
            var ids = string.Join(",", list.Select(r => r.UniverseId).Distinct());
            foreach (var g in (await Get("https://games.roblox.com/v1/games?universeIds=" + ids)).GetProperty("data").EnumerateArray())
                info[g.GetProperty("id").GetInt64()] = (g.GetProperty("name").GetString() ?? "", g.GetProperty("creator").GetProperty("name").GetString() ?? "", g.GetProperty("playing").GetInt64());
            foreach (var v in (await Get("https://games.roblox.com/v1/games/votes?universeIds=" + ids)).GetProperty("data").EnumerateArray())
            {
                long up = v.GetProperty("upVotes").GetInt64(), down = v.GetProperty("downVotes").GetInt64();
                votes[v.GetProperty("id").GetInt64()] = up + down > 0 ? $"{up * 100 / (up + down)}% liked" : "";
            }
            foreach (var t in (await Get($"https://thumbnails.roblox.com/v1/games/multiget/thumbnails?universeIds={ids}&countPerUniverse=1&size=768x432&format=Png&isCircular=false")).GetProperty("data").EnumerateArray())
                if (t.GetProperty("thumbnails").EnumerateArray().FirstOrDefault() is { ValueKind: JsonValueKind.Object } th && th.GetProperty("imageUrl").GetString() is { Length: > 0 } url)
                    pics[t.GetProperty("universeId").GetInt64()] = url;
            try { s.Save(); } catch (System.IO.IOException) { }  // keep the universe ids
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException or KeyNotFoundException or InvalidOperationException) { }  // offline: cards show the place id
        RecentList.ItemsSource = list.Select(r =>
        {
            var has = info.TryGetValue(r.UniverseId, out var g);
            var stats = has ? string.Join("  ·  ", new[] { votes.GetValueOrDefault(r.UniverseId, ""), $"{g.playing:N0} playing" }.Where(x => x != "")) : "";
            return new Card(r.PlaceId, has ? g.name : $"Place {r.PlaceId}", has ? "by " + g.creator : "", stats,
                            "Joined: " + r.Joined.ToString("MMM d, yyyy h:mm tt"), pics.GetValueOrDefault(r.UniverseId));
        }).ToList();
    }

    void Start(long? place)
    {
        StatusText.Text = "";
        try { Launcher.Start(place); wasRunning = true; Running(true); }
        catch (Exception ex) { StatusText.Text = "Could not start loader.exe: " + ex.Message; }
    }

    void Play_Click(object sender, RoutedEventArgs e) => Start(null);
    void PlayGame_Click(object sender, RoutedEventArgs e) => Start((long)((FrameworkElement)sender).Tag);
    void Stop_Click(object sender, RoutedEventArgs e) => Launcher.Stop();
}
