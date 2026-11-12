using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Text.Json;

namespace Skatestrap;

// Starts loader.exe with the SKATESTRAP_* switches the settings ask for, and keeps its output in run.log.
public static class Launcher
{
    public static Process? Running;
    public static event Action<string>? Line;   // one log line (cookie lines never reach here)
    public static string LogPath => Path.Combine(Settings.Root, "run.log");

    static readonly System.Text.RegularExpressions.Regex Joining = new(@"launch: nativeAppBridgeV2StartGameWithParam\(place (\d+)");

    // placeId null = the Roblox app home screen; a number joins that game straight away
    public static void Start(long? placeId = null)
    {
        var s = Settings.Current;
        s.Save();
        var loaderDir = Path.Combine(Settings.Root, "loader");
        var psi = new ProcessStartInfo(Path.Combine(loaderDir, "loader.exe"), @"..\extract\libroblox.so ..\patches.bin")
        {
            WorkingDirectory = loaderDir,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        var env = psi.Environment;
        env["SKATESTRAP_WATCH_MS"] = "0";  // unset = the loader quits after 10 s
        env["SKATESTRAP_SELF_UPDATES"] = "1";  // this launcher updates itself; the loader's swap is only for launchers before 0.0.12
        var login = Login.Cookie();  // handed to the loader in its environment, never written to disk in plain text
        if (login != null)
        {
            env["SKATESTRAP_COOKIE"] = login;
            if (s.UserId != "") env["SKATESTRAP_USER_ID"] = s.UserId;
            if (s.Username != "") env["SKATESTRAP_USERNAME"] = s.Username;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Login.Saved)!);
        env["SKATESTRAP_LOGIN_FILE"] = Login.Saved;  // the loader saves a sign-in made on Roblox's own screen here (DPAPI)
        if (placeId != null) env["SKATESTRAP_PLACE_ID"] = placeId.ToString();
        if (s.Renderer == "vulkan") env["SKATESTRAP_VULKAN"] = "1";
        if (s.Renderer == "native") env["SKATESTRAP_GL"] = "native";
        if (s.FpsCap != "") env["SKATESTRAP_FPS_CAP"] = s.FpsCap;
        if (!s.PerfTweaks) env["SKATESTRAP_PERF"] = "0";
        if (s.RenderScale != "") env["SKATESTRAP_RENDER_SCALE"] = s.RenderScale;
        if (!s.Fsr) env["SKATESTRAP_UPSCALE"] = "linear";
        if (s.FpsStats) env["SKATESTRAP_FPS"] = "1";
        if (s.Profiler) env["SKATESTRAP_PROF"] = "1";
        if (s.Fullscreen) env["SKATESTRAP_FULLSCREEN"] = "1";
        if (s.PsIcons) env["SKATESTRAP_PS_ICONS"] = "1";
        if (s.Mods && Directory.Exists(Settings.ModsDir)) env["SKATESTRAP_MODS"] = Settings.ModsDir;
        if (s.FreezeDump) env["SKATESTRAP_FREEZE_DUMP"] = Path.Combine(Settings.Root, "freeze.dmp");
        var flags = new Dictionary<string, string>(s.Flags);
        // GPU wait per 5 s fell 188 -> 4 ms with this (bench vkq1); a flag typed on the Flags page wins
        if (s.LowGraphics) flags.TryAdd("DFIntDebugFRMQualityLevelOverride", "1");
        // Graphics page presets: the same flag names Bloxstrap / Voidstrap use (the Android app reads the same flag list)
        if (s.NoGrass) { flags.TryAdd("FIntFRMMinGrassDistance", "0"); flags.TryAdd("FIntFRMMaxGrassDistance", "0"); flags.TryAdd("FIntRenderGrassDetailStrands", "0"); }
        if (s.StillGrass) flags.TryAdd("FIntGrassMovementReducedMotionFactor", "0");
        if (s.Msaa != "") flags.TryAdd("FIntDebugForceMSAASamples", s.Msaa);
        if (s.Textures != "") { flags.TryAdd("DFFlagTextureQualityOverrideEnabled", "True"); flags.TryAdd("DFIntTextureQualityOverride", s.Textures); }
        if (flags.Count > 0)
        {
            var flagsFile = Path.Combine(Settings.Root, "skatestrap_flags.json");
            File.WriteAllText(flagsFile, JsonSerializer.Serialize(flags));
            env["SKATESTRAP_FLAGS_FILE"] = flagsFile;
        }

        var log = new StreamWriter(LogPath, false) { AutoFlush = true };
        if (Updater.LastError != null) log.WriteLine("updater: " + Updater.LastError);
        var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
        DataReceivedEventHandler onLine = (_, e) =>
        {
            if (e.Data == null) return;
            lock (log) log.WriteLine(e.Data);
            var join = Joining.Match(e.Data);
            if (join.Success && long.TryParse(join.Groups[1].Value, out var place)) s.Played(place);
            if (!e.Data.Contains("ROBLOSECURITY", StringComparison.OrdinalIgnoreCase)) Line?.Invoke(e.Data);
        };
        p.OutputDataReceived += onLine;
        p.ErrorDataReceived += onLine;
        p.Exited += (_, _) =>
        {
            p.WaitForExit(); lock (log) { log.WriteLine($"-- loader exited ({p.ExitCode}, 0x{p.ExitCode:X8}) --"); log.Dispose(); } Running = null; Line?.Invoke($"-- loader exited ({p.ExitCode}) --");
            if (s.SendLog) _ = SendLog(login, s.Username);
        };
        p.Start();
        try { p.PriorityClass = s.Priority switch { "high" => ProcessPriorityClass.High, "abovenormal" => ProcessPriorityClass.AboveNormal, _ => ProcessPriorityClass.Normal }; }
        catch (Exception ex) { Line?.Invoke("-- priority not set: " + ex.Message + " --"); }
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        Running = p;
    }

    // Debug > Send run.log: upload the log to our server (write-only there). Login lines and the cookie itself never leave.
    public static string Scrub(string text, string? cookie)
    {
        var keep = text.Split('\n').Where(l => !l.Contains("ROBLOSECURITY", StringComparison.OrdinalIgnoreCase));
        var clean = string.Join("\n", keep);
        if (!string.IsNullOrEmpty(cookie)) clean = clean.Replace(cookie, "[login removed]");
        const int Max = 1_500_000;  // server takes 2 MB; the end of the log is the part that matters
        return clean.Length > Max ? "[start cut]\n" + clean[^Max..] : clean;
    }

    static async Task SendLog(string? cookie, string username)
    {
        try
        {
            var who = new string(username.Where(c => char.IsAsciiLetterOrDigit(c) || c == '_').Take(20).ToArray());
            var name = $"{(who == "" ? "anon" : who)}-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Random.Shared.Next(0x10000):x4}.log";
            var body = $"Skatestrap {Updater.Local()} | {Environment.OSVersion} | {Environment.ProcessorCount} threads\n" + Scrub(await File.ReadAllTextAsync(LogPath), cookie);
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(60) };
            var res = await http.PutAsync("https://krashscripts.xyz/logs/" + name, new StringContent(body));
            Line?.Invoke(res.IsSuccessStatusCode ? $"-- run.log sent as {name} --" : $"-- run.log not sent (server said {(int)res.StatusCode}) --");
        }
        catch (Exception ex) { Line?.Invoke("-- run.log not sent: " + ex.Message + " --"); }
    }

    public static void Stop()
    {
        try { Running?.Kill(); } catch (InvalidOperationException) { } // already gone
    }

    // Who a login cookie belongs to, from Roblox itself. The cookie only ever goes to roblox.com.
    public static async Task<(string id, string name)?> WhoIs(string raw)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
        var req = new HttpRequestMessage(HttpMethod.Get, "https://users.roblox.com/v1/users/authenticated");
        req.Headers.Add("Cookie", ".ROBLOSECURITY=" + raw);
        var res = await http.SendAsync(req);
        if (!res.IsSuccessStatusCode) return null;
        using var doc = JsonDocument.Parse(await res.Content.ReadAsStringAsync());
        return (doc.RootElement.GetProperty("id").GetInt64().ToString(), doc.RootElement.GetProperty("name").GetString() ?? "");
    }
}
