using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;

namespace Skatestrap;

// On launch: ask the server (manifest.json) for the pinned Roblox APK version and our own loader build, then make the
// install match it. Like a Roblox bootstrapper, the Roblox APK is downloaded from a store mirror on the player's own
// machine and extracted here; we never host Roblox's binary. The extracted libroblox.so is checked against a SHA-256
// pinned in our manifest, so a mirror that tampered with the APK is rejected (and it also guarantees the loader's byte
// offsets match). Never blocks play: if the manifest can't be fetched and an APK is already extracted, play continues.
public static class Updater
{
    const string ManifestUrl = "https://krashscripts.xyz/update/manifest.json";
    static string VersionFile => Path.Combine(Settings.Root, "version.txt");   // our tool version
    static string ApkFile => Path.Combine(Settings.Root, "apk.txt");           // Roblox APK version now extracted
    static string SoPath => Path.Combine(Settings.Root, "extract", "libroblox.so");

    public static string? LastError;   // why the last check failed; shown on the Play page and written into run.log

    public static string Local() => Read(VersionFile, "0.0.0");
    static string Read(string p, string dflt) { try { return File.ReadAllText(p).Trim(); } catch { return dflt; } }

    public static async Task CheckAndApply(Action<string> status)
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
            using var doc = JsonDocument.Parse(await http.GetStringAsync(ManifestUrl));
            var root = doc.RootElement;

            // 1. Roblox APK: fetch + extract if the pinned version isn't the one we have (or nothing is extracted yet).
            if (root.TryGetProperty("apk", out var apk))
                await SyncApk(apk, status);

            // 2. our own files (loader.exe, patches.bin): download if the manifest version is newer.
            var version = root.GetProperty("version").GetString() ?? "";
            if (root.TryGetProperty("files", out var files) && Newer(version, Local()))
            {
                status($"Downloading update v{version}…");
                var swaps = new List<(string tmp, string dest)>();
                var temps = new List<string>();
                try
                {
                    foreach (var f in files.EnumerateArray())
                    {
                        var name = f.GetProperty("name").GetString()!;
                        var dest = Path.Combine(Settings.Root, name.Replace('/', Path.DirectorySeparatorChar));
                        var tmp = dest + ".new";
                        temps.Add(tmp);
                        var bytes = await http.GetByteArrayAsync(f.GetProperty("url").GetString()!);
                        if (Hex(SHA256.HashData(bytes)) != f.GetProperty("sha256").GetString()!.ToLowerInvariant())
                            throw new Exception($"checksum mismatch for {name}");
                        Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                        await File.WriteAllBytesAsync(tmp, bytes);
                        swaps.Add((tmp, dest));
                    }
                    foreach (var (tmp, dest) in swaps) File.Move(tmp, dest, overwrite: true);
                    File.WriteAllText(VersionFile, version);
                }
                catch { foreach (var t in temps) { try { File.Delete(t); } catch { } } throw; }
            }
            status("");
        }
        catch (Exception ex)
        {
            LastError = ex.Message;
            // Offline / server down / bad download. If an APK is already extracted, play on what we have; else surface it.
            status(File.Exists(SoPath) ? "" : "Could not reach the update server, and Roblox is not downloaded yet. Check your connection and reopen.");
        }
    }

    // Download the pinned Roblox APK from the store mirror and extract libroblox.so, the helper .so files, and assets.
    static async Task SyncApk(JsonElement apk, Action<string> status)
    {
        var id = apk.GetProperty("id").GetString()!;            // com.roblox.client
        var ver = apk.GetProperty("version").GetString()!;      // e.g. 2.746.1266
        var arch = apk.GetProperty("arch").GetString() ?? "x86_64";
        var soSha = apk.GetProperty("so_sha256").GetString()!.ToLowerInvariant();

        if (Read(ApkFile, "") == ver && File.Exists(SoPath)) return;   // already on this APK

        var apkeep = ApkeepPath();
        if (apkeep == null) throw new Exception("apkeep tool not found");

        status($"Downloading Roblox {ver}… (one time, large)");
        var tmpDir = Path.Combine(Path.GetTempPath(), "skatestrap-apk");
        if (Directory.Exists(tmpDir)) Directory.Delete(tmpDir, true);
        Directory.CreateDirectory(tmpDir);

        var psi = new ProcessStartInfo(apkeep,
            $"-a {id}@{ver} -d apk-pure -o \"acknowledge_dangers=true,arch={arch}\" \"{tmpDir}\"")
            { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
        using (var p = Process.Start(psi)!)
        {
            // both pipes are read to the end: an unread pipe fills up and the tool then waits for ever
            var outText = p.StandardOutput.ReadToEndAsync();
            var errText = p.StandardError.ReadToEndAsync();
            await p.WaitForExitAsync();
            var said = ((await errText) + " " + (await outText)).Trim();
            if (p.ExitCode != 0) throw new Exception($"apkeep download failed ({p.ExitCode}): {(said.Length > 600 ? said[^600..] : said)}");
        }
        var apkPath = Directory.GetFiles(tmpDir, "*.apk").FirstOrDefault()
                      ?? throw new Exception("no APK downloaded: " + string.Join(", ", Directory.GetFileSystemEntries(tmpDir).Select(Path.GetFileName)));

        status("Extracting Roblox…");
        var extract = Path.Combine(Settings.Root, "extract");
        var assetBase = Path.Combine(Settings.Root, "loader", "root", "data", "app", "com.roblox.client");
        Directory.CreateDirectory(extract);
        using (var zip = ZipFile.OpenRead(apkPath))
        {
            foreach (var e in zip.Entries)
            {
                string? dest = null;
                if (e.FullName.StartsWith($"lib/{arch}/") && e.FullName.EndsWith(".so"))
                    dest = Path.Combine(extract, Path.GetFileName(e.FullName));
                else if (e.FullName.StartsWith("assets/") && !e.FullName.EndsWith("/"))
                    dest = Path.Combine(assetBase, e.FullName.Replace('/', Path.DirectorySeparatorChar));
                if (dest == null) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                e.ExtractToFile(dest, overwrite: true);
            }
        }
        try { Directory.Delete(tmpDir, true); } catch { }

        // Reject a tampered mirror: the extracted engine must match the hash we pinned.
        if (!File.Exists(SoPath) || Hex(SHA256.HashData(await File.ReadAllBytesAsync(SoPath))) != soSha)
        {
            try { File.Delete(SoPath); } catch { }
            throw new Exception("downloaded libroblox.so failed the checksum; refusing it");
        }
        File.WriteAllText(ApkFile, ver);
    }

    static string? ApkeepPath()
    {
        var dir = Path.Combine(Settings.Root, "tools");
        foreach (var n in new[] { "apkeep.exe", "apkeep-x86_64-pc-windows-msvc.exe" })
        {
            var p = Path.Combine(dir, n);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    static string Hex(byte[] b) => Convert.ToHexString(b).ToLowerInvariant();

    // true if version a is newer than version b (three-part "x.y.z", leading "v" ignored)
    static bool Newer(string a, string b)
    {
        int[] pa = Parse(a), pb = Parse(b);
        for (int i = 0; i < 3; i++) if (pa[i] != pb[i]) return pa[i] > pb[i];
        return false;
    }

    static int[] Parse(string v)
    {
        var p = new int[3];
        var parts = v.TrimStart('v', 'V').Split('.');
        for (int i = 0; i < 3 && i < parts.Length; i++) int.TryParse(parts[i], out p[i]);
        return p;
    }
}
