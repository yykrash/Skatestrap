using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Windows;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.Wpf;

namespace Skatestrap;

// Login: normally Roblox's own sign-in screen inside the game window (the loader saves the session to login.bin, like Mocktail).
// Fallback: the real roblox.com login page in a WebView2 window. Either way Skatestrap never sees the password.
// The session it ends with is kept encrypted for this Windows user (DPAPI) and only ever goes to roblox.com and the loader.
public static class Login
{
    static readonly string Dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Skatestrap");
    public static string Saved => Path.Combine(Dir, "login.bin");

    public static bool Has => File.Exists(Saved);
    public static void Forget() => File.Delete(Saved);

    // Account switcher: every account signed in here keeps its own DPAPI copy in accounts\<userId>.bin;
    // switching copies it over login.bin (the one the game uses). Ids are Roblox user ids (digits only).
    static string Account(string id) => id.Length > 0 && id.All(char.IsAsciiDigit) ? Path.Combine(Dir, "accounts", id + ".bin") : throw new ArgumentException("bad user id");
    public static string[] Accounts() => Directory.Exists(Path.Combine(Dir, "accounts"))
        ? Directory.GetFiles(Path.Combine(Dir, "accounts"), "*.bin").Select(f => Path.GetFileNameWithoutExtension(f)).ToArray() : [];
    public static void Keep(string id) { Directory.CreateDirectory(Path.Combine(Dir, "accounts")); File.Copy(Saved, Account(id), true); }
    public static void Use(string id) => File.Copy(Account(id), Saved, true);
    public static void Drop(string id) => File.Delete(Account(id));

    public static string? Cookie()
    {
        try { return Encoding.UTF8.GetString(ProtectedData.Unprotect(File.ReadAllBytes(Saved), null, DataProtectionScope.CurrentUser)); }
        catch (Exception e) when (e is IOException or CryptographicException) { return null; }  // not logged in, or saved by another Windows user
    }

    // Opens the login window; returns who logged in, or null if the window was closed first.
    public static (string id, string name)? Show(Window? owner)
    {
        var web = new WebView2();
        var win = new Window { Title = "Log in to Roblox (roblox.com)", Width = 520, Height = 780, Owner = owner, WindowStartupLocation = WindowStartupLocation.CenterOwner, Content = web };
        (string id, string name)? got = null;
        var busy = false;
        async void Look()
        {
            if (busy || got != null) return;
            busy = true;
            try
            {
                var jar = web.CoreWebView2.CookieManager;
                var c = (await jar.GetCookiesAsync("https://www.roblox.com")).FirstOrDefault(x => x.Name == ".ROBLOSECURITY");
                var who = c == null ? null : await Launcher.WhoIs(c.Value);
                if (who == null) return;  // not logged in yet (or still at the 2-step check)
                Directory.CreateDirectory(Dir);
                File.WriteAllBytes(Saved, ProtectedData.Protect(Encoding.UTF8.GetBytes(c!.Value), null, DataProtectionScope.CurrentUser));
                jar.DeleteAllCookies();  // login.bin is the only copy
                got = who;
                win.Close();
            }
            catch (Exception ex) when (ex is IOException or System.Net.Http.HttpRequestException or TaskCanceledException) { MessageBox.Show(win, "Could not finish the login: " + ex.Message); }
            finally { busy = false; }
        }
        win.Loaded += async (_, _) =>
        {
            try
            {
                await web.EnsureCoreWebView2Async(await CoreWebView2Environment.CreateAsync(null, Path.Combine(Dir, "WebView2")));
                web.CoreWebView2.CookieManager.DeleteAllCookies();  // always a fresh login, so another account can be picked
                web.CoreWebView2.NavigationCompleted += (_, _) => Look();
                web.CoreWebView2.SourceChanged += (_, _) => Look();
                web.CoreWebView2.Navigate("https://www.roblox.com/login");
            }
            catch (Exception ex) { MessageBox.Show(win, "The login window needs the Microsoft Edge WebView2 Runtime: " + ex.Message); win.Close(); }
        };
        win.ShowDialog();
        return got;
    }
}
