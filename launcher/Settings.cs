using System.IO;
using System.Text.Json;

namespace Skatestrap;

// Everything Skatestrap remembers, saved as skatestrap.json in the Skatestrap folder.
public class Settings
{
    public string UserId { get; set; } = "";
    public string Username { get; set; } = "";
    public Dictionary<string, string> Accounts { get; set; } = new();  // saved accounts: user id -> username (sign-ins in Login accounts\)
    public string Renderer { get; set; } = "vulkan";       // vulkan | angle | native
    public string FpsCap { get; set; } = "unlimited";      // "" = Roblox default, a number, or unlimited
    public bool PerfTweaks { get; set; } = true;           // SKATESTRAP_PERF (Mocktail throughput flags)
    public bool LowGraphics { get; set; }                  // forces Roblox graphics quality 1 (laptops with built-in graphics)
    public bool Fsr { get; set; } = true;                  // false = SKATESTRAP_UPSCALE=linear (plain stretch at render scale < 1)
    public string RenderScale { get; set; } = "";          // SKATESTRAP_RENDER_SCALE (Vulkan only): "" = full size, else 0.5 / 0.75 ...
    public Dictionary<string, string> Flags { get; set; } = new();
    public bool FpsStats { get; set; }
    public bool Profiler { get; set; }
    public bool FreezeDump { get; set; }
    public bool SendLog { get; set; }                      // upload run.log to krashscripts.xyz when the game closes
    public bool Fullscreen { get; set; }                   // SKATESTRAP_FULLSCREEN: start borderless over the whole screen (F11 / Alt+Enter switch)
    public bool PsIcons { get; set; }                      // SKATESTRAP_PS_ICONS: every pad reports as PS4, so Roblox shows PlayStation buttons
    public string Priority { get; set; } = "normal";       // Windows priority of the game: normal | abovenormal | high
    public bool NoGrass { get; set; }                      // grass distance 0 (flags below; a flag typed on the Flags page wins)
    public bool StillGrass { get; set; }                   // grass does not sway
    public string Msaa { get; set; } = "";                 // "" = Roblox decides, else 1 / 2 / 4 / 8 samples
    public string Textures { get; set; } = "";             // "" = Roblox decides, else 0 (lowest) .. 3 (highest)

    public List<RecentGame> Recent { get; set; } = new();  // games joined, newest first (Play page "Continue playing"); lock it to touch
    public bool Mods { get; set; } = true;                // SKATESTRAP_MODS: files in ModsDir replace the game's (Bloxstrap "Modifications" layout)
    public static string ModsDir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Skatestrap", "Modifications");
    public static readonly string Root = FindRoot();
    static string PathOf => Path.Combine(Root, "skatestrap.json");
    public static Settings Current { get; } = Load();

    // The Skatestrap folder: the first parent of the exe that holds loader\loader.exe.
    static string FindRoot()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "loader", "loader.exe"))) return d.FullName;
        return AppContext.BaseDirectory;
    }

    public bool GpuChecked { get; set; }                   // first-run laptop setup done (never redone, so the player's own choices stay)

    static Settings Load()
    {
        Settings s = new();
        try { if (File.Exists(PathOf)) s = JsonSerializer.Deserialize<Settings>(File.ReadAllText(PathOf)) ?? new(); }
        catch { } // a broken file starts fresh rather than blocking the launcher
        if (!s.GpuChecked) s.AutoSetup();
        return s;
    }

    // Built-in graphics only (no separate card): Low graphics on + 75% render resolution.
    void AutoSetup()
    {
        string kind = "";
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(Path.Combine(Root, "loader", "loader.exe"), "--gpu")
                { UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true };
            using var p = System.Diagnostics.Process.Start(psi)!;
            var output = p.StandardOutput.ReadToEnd();
            if (!p.WaitForExit(10000)) { p.Kill(); return; }
            kind = output.Split('\n').Select(l => l.Trim()).LastOrDefault(l => l.StartsWith("gpu: "))?[5..] ?? "";
        }
        catch { return; } // no loader / no Vulkan: try again next start
        if (kind == "integrated") { LowGraphics = true; RenderScale = "0.75"; }
        GpuChecked = true;
        try { Save(); } catch { }
    }

    public void Save()
    {
        lock (Recent) File.WriteAllText(PathOf, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }

    // the loader printed that it is joining this place (picked in the app or from a card)
    public void Played(long placeId)
    {
        lock (Recent)
        {
            var old = Recent.FirstOrDefault(r => r.PlaceId == placeId);
            Recent.RemoveAll(r => r.PlaceId == placeId);
            Recent.Insert(0, new RecentGame { PlaceId = placeId, UniverseId = old?.UniverseId ?? 0, Joined = DateTime.Now });
            if (Recent.Count > 12) Recent.RemoveRange(12, Recent.Count - 12);
        }
        try { Save(); } catch (IOException) { }
    }
}

public class RecentGame
{
    public long PlaceId { get; set; }
    public long UniverseId { get; set; }  // 0 = not looked up yet
    public DateTime Joined { get; set; }
}
