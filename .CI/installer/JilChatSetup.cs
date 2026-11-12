// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

// JilChat Chatterino's installer: a single program holding the app as an
// embedded zip. It draws its whole window itself on a per-pixel-alpha surface,
// which is what allows the soft shadow, rounded corners and glow. The window
// follows the installer of Solrino (MIT), in black and white.
//
// Built by build-installer.ps1 with the C# compiler that ships with Windows,
// so it's C# 5: no string interpolation, no ?. and no expression bodies.
//
// Switches:
//   --dest <folder>   install there instead of asking (the updater passes the
//                     folder the app runs from)
//   --update          what the app's updater uses: waits for the app to close,
//                     installs without questions and keeps the shortcut choice.
//                     The Inno Setup switches older versions pass (/SILENT ...)
//                     mean the same.
//   --auto            install right away, without the first screen
//   --no-launch       don't start the app afterwards
//   --no-register     no shortcuts and no entry in Installed apps (for tests)
//   --desktop / --no-desktop  create the desktop shortcut or not
//   --screenshot <folder>  saves each screen as a picture there, then quits

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace JilChatSetup
{
    enum Screen { Welcome, Installing, Done, Problem }

    static class App
    {
        public const string Name = "JilChat Chatterino";
        public const string Exe = "JilChatChatterino.exe";
        public const string Process = "JilChatChatterino";
        public const string AppUserModelId = "Jilence.JilChatChatterino";
        /// Next to the program, and what Installed apps runs to remove it
        public const string Uninstaller = "Uninstall JilChat Chatterino.exe";
        /// Our entry in Installed apps
        public const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\JilChatChatterino";
        /// The entry the Inno Setup installer of older versions made
        public const string InnoUninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\{F5FE6614-04D4-4D32-8600-0ABA0AC113A4}_is1";
        /// Names the program and its shortcuts had before
        public static readonly string[] OldNames = { "Leafyrino" };
    }

    class Options
    {
        public string Destination;
        public bool DestinationGiven;
        public bool Register = true;
        public bool Launch = true;
        public bool Auto;
        public bool Update;
        /// null: keep what's there (on an update) or the default
        public bool? Desktop;
        /// Set when this copy was started elevated by another one
        public bool Elevated;
        /// Where to save pictures of the screens, instead of installing
        public string ScreenshotFolder;

        public static Options Parse(string[] args)
        {
            var o = new Options();
            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i];
                if (a == "--dest" && i + 1 < args.Length) { o.Destination = args[++i]; o.DestinationGiven = true; }
                else if (a == "--no-register") { o.Register = false; }
                else if (a == "--no-launch") { o.Launch = false; }
                else if (a == "--auto") { o.Auto = true; }
                else if (a == "--update") { o.Auto = true; o.Update = true; }
                else if (a == "--desktop") { o.Desktop = true; }
                else if (a == "--no-desktop") { o.Desktop = false; }
                else if (a == "--elevated") { o.Elevated = true; }
                else if (a == "--screenshot" && i + 1 < args.Length) { o.ScreenshotFolder = args[++i]; }
                // What the updater of versions with the Inno Setup installer passes
                else if (a.Equals("/SILENT", StringComparison.OrdinalIgnoreCase) ||
                         a.Equals("/VERYSILENT", StringComparison.OrdinalIgnoreCase))
                {
                    o.Auto = true;
                    o.Update = true;
                }
                else if (a.StartsWith("/DIR=", StringComparison.OrdinalIgnoreCase))
                {
                    o.Destination = a.Substring(5).Trim('"');
                    o.DestinationGiven = true;
                }
            }
            if (string.IsNullOrEmpty(o.Destination)) { o.Destination = ExistingInstall() ?? DefaultFolder(); }
            return o;
        }

        public static string DefaultFolder()
        {
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"Programs\" + App.Name);
        }

        /// Where it's installed already, by us or by the Inno Setup installer
        static string ExistingInstall()
        {
            foreach (var hive in new[] { Registry.CurrentUser, Registry.LocalMachine })
            {
                foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
                {
                    foreach (var name in new[] { App.UninstallKey, App.InnoUninstallKey })
                    {
                        try
                        {
                            using (var root = RegistryKey.OpenBaseKey(
                                       hive == Registry.CurrentUser ? RegistryHive.CurrentUser : RegistryHive.LocalMachine, view))
                            using (var key = root.OpenSubKey(name))
                            {
                                if (key == null) { continue; }
                                var location = key.GetValue("InstallLocation") as string;
                                if (!string.IsNullOrEmpty(location) &&
                                    File.Exists(Path.Combine(location, App.Exe)))
                                {
                                    return location.TrimEnd('\\');
                                }
                            }
                        }
                        catch (System.Security.SecurityException) { }
                        catch (UnauthorizedAccessException) { }
                        catch (IOException) { }
                    }
                }
            }
            return null;
        }

        /// The switches for a second copy that does the same
        public string ToArguments(bool desktop)
        {
            var args = new StringBuilder();
            args.Append("--dest \"").Append(Destination.TrimEnd('\\')).Append("\" --auto --elevated --no-launch");
            args.Append(desktop ? " --desktop" : " --no-desktop");
            if (Update) { args.Append(" --update"); }
            if (!Register) { args.Append(" --no-register"); }
            return args.ToString();
        }
    }

    /// Does the actual install on a background thread.
    class Installer
    {
        readonly Options options;
        readonly bool desktopShortcut;
        public volatile string CurrentFile = "";
        public volatile bool Finished;
        public volatile string Error;
        long done;
        long total = 1;

        public double Progress { get { return Math.Min(1.0, (double)Interlocked.Read(ref done) / Interlocked.Read(ref total)); } }

        public Installer(Options options, bool desktopShortcut)
        {
            this.options = options;
            this.desktopShortcut = desktopShortcut;
        }

        public void Start()
        {
            var thread = new Thread(Run);
            thread.IsBackground = true;
            thread.Start();
        }

        void Run()
        {
            try
            {
                Install();
            }
            catch (IOException e) { Error = "A file couldn't be written. Close " + App.Name + " and try again.\n" + e.Message; }
            catch (UnauthorizedAccessException) { Error = "Windows blocked access to the install folder. Choose another folder or try again."; }
            catch (Exception e) { Error = e.Message; }
            Finished = true;
        }

        void Install()
        {
            var assembly = Assembly.GetExecutingAssembly();
            string root = Path.GetFullPath(options.Destination);
            string rootPrefix = root.TrimEnd('\\') + "\\";
            Directory.CreateDirectory(root);

            using (var stream = assembly.GetManifestResourceStream("app.zip"))
            {
                if (stream == null) { throw new Exception("This setup file is damaged. Please download it again."); }
                using (var zip = new ZipArchive(stream, ZipArchiveMode.Read))
                {
                    long sum = 1;
                    foreach (var entry in zip.Entries) { sum += entry.Length; }
                    Interlocked.Exchange(ref total, sum);

                    var buffer = new byte[256 * 1024];
                    foreach (var entry in zip.Entries)
                    {
                        string relative = entry.FullName.Replace('/', '\\');
                        string path = Path.GetFullPath(Path.Combine(root, relative));
                        if (!path.StartsWith(rootPrefix, StringComparison.OrdinalIgnoreCase))
                        {
                            throw new Exception("This setup file is damaged. Please download it again.");
                        }
                        if (relative.EndsWith("\\")) { Directory.CreateDirectory(path); continue; }

                        CurrentFile = relative;
                        Directory.CreateDirectory(Path.GetDirectoryName(path));
                        using (var input = entry.Open())
                        using (var output = File.Create(path))
                        {
                            int read;
                            while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
                            {
                                output.Write(buffer, 0, read);
                                Interlocked.Add(ref done, read);
                            }
                        }
                    }
                }
            }

            string exe = Path.Combine(root, App.Exe);
            if (!File.Exists(exe)) { throw new Exception(App.Exe + " wasn't installed. Please download the setup again."); }

            InstallRuntimeIfNeeded(assembly);

            CurrentFile = "Cleaning up";
            foreach (var old in App.OldNames)
            {
                TryDelete(Path.Combine(root, old + ".exe"));
            }
            RemoveInnoSetup(root);
            // The cache is rebuilt anyway, and an old one can confuse a new version
            TryDeleteFolder(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), @"Chatterino2\Cache"));

            using (var stream = assembly.GetManifestResourceStream("uninstall.exe"))
            using (var output = File.Create(Path.Combine(root, App.Uninstaller)))
            {
                stream.CopyTo(output);
            }
            // Earlier versions of this setup uninstalled through a PowerShell
            // script, which antivirus apps find suspicious
            TryDelete(Path.Combine(root, "uninstall.ps1"));

            if (options.Register)
            {
                CurrentFile = "Creating shortcuts";
                bool machine = IsElevated() && !root.StartsWith(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), StringComparison.OrdinalIgnoreCase);
                string programs = Environment.GetFolderPath(machine
                    ? Environment.SpecialFolder.CommonPrograms : Environment.SpecialFolder.Programs);
                string desktop = Environment.GetFolderPath(machine
                    ? Environment.SpecialFolder.CommonDesktopDirectory : Environment.SpecialFolder.DesktopDirectory);
                foreach (var old in App.OldNames)
                {
                    TryDelete(Path.Combine(programs, old + ".lnk"));
                    TryDelete(Path.Combine(desktop, old + ".lnk"));
                }
                Shortcut.Create(Path.Combine(programs, App.Name + ".lnk"), exe, root);
                string desktopLink = Path.Combine(desktop, App.Name + ".lnk");
                if (desktopShortcut) { Shortcut.Create(desktopLink, exe, root); }
                else { TryDelete(desktopLink); }
                Register(exe, root, machine);
            }
            CurrentFile = "";
        }

        /// Installs the Visual C++ runtime the setup carries when Windows has
        /// none or an older one, like the Inno Setup installer did. Its own
        /// installer asks for administrator rights.
        void InstallRuntimeIfNeeded(Assembly assembly)
        {
            string shipped;
            using (var stream = assembly.GetManifestResourceStream("vcrt.txt"))
            {
                if (stream == null) { return; }
                using (var reader = new StreamReader(stream)) { shipped = reader.ReadToEnd().Trim(); }
            }
            // "<arch> <minor>", e.g. "x64 44"
            var parts = shipped.Split(' ');
            int shippedMinor;
            if (parts.Length != 2 || !int.TryParse(parts[1], out shippedMinor)) { return; }

            try
            {
                using (var baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
                using (var key = baseKey.OpenSubKey(@"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\" + parts[0]))
                {
                    var minor = key != null ? key.GetValue("Minor") : null;
                    if (minor is int && (int)minor >= shippedMinor) { return; }
                }
            }
            catch (System.Security.SecurityException) { }
            catch (UnauthorizedAccessException) { }
            catch (IOException) { }

            CurrentFile = "Installing the Visual C++ runtime";
            string path = Path.Combine(Path.GetTempPath(), "jilchat-vc_redist-" + Guid.NewGuid().ToString("N") + ".exe");
            using (var stream = assembly.GetManifestResourceStream("vc_redist.exe"))
            {
                if (stream == null) { return; }
                using (var output = File.Create(path)) { stream.CopyTo(output); }
            }
            try
            {
                var start = new ProcessStartInfo(path, "/install /passive /norestart");
                start.UseShellExecute = true;
                using (var process = System.Diagnostics.Process.Start(start))
                {
                    process.WaitForExit();
                    // 1638: a newer one is installed, 3010: needs a restart
                    int code = process.ExitCode;
                    if (code != 0 && code != 1638 && code != 3010)
                    {
                        throw new Exception("The Visual C++ runtime couldn't be installed (error " + code + "). " +
                                            App.Name + " may not start without it.");
                    }
                }
            }
            catch (System.ComponentModel.Win32Exception)
            {
                throw new Exception("The Visual C++ runtime wasn't installed, because Windows didn't allow it. " +
                                    App.Name + " may not start without it.");
            }
            finally
            {
                try { File.Delete(path); }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }

        /// Takes over a folder that the Inno Setup installer of older versions
        /// filled: its uninstaller would remove our files
        static void RemoveInnoSetup(string root)
        {
            foreach (var name in new[] { "unins000.exe", "unins000.dat", "unins000.msg" })
            {
                TryDelete(Path.Combine(root, name));
            }
            foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
            {
                foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
                {
                    try
                    {
                        using (var baseKey = RegistryKey.OpenBaseKey(hive, view))
                        {
                            baseKey.DeleteSubKeyTree(App.InnoUninstallKey, false);
                        }
                    }
                    catch (System.Security.SecurityException) { }
                    catch (UnauthorizedAccessException) { }
                    catch (IOException) { }
                }
            }
        }

        static void Register(string exe, string root, bool machine)
        {
            var hive = machine ? Registry.LocalMachine : Registry.CurrentUser;
            using (var key = hive.CreateSubKey(App.UninstallKey))
            {
                key.SetValue("DisplayName", App.Name);
                key.SetValue("DisplayIcon", exe + ",0");
                key.SetValue("DisplayVersion", FileVersionInfo.GetVersionInfo(exe).ProductVersion ?? "");
                key.SetValue("Publisher", "Jilence");
                key.SetValue("InstallLocation", root);
                key.SetValue("NoModify", 1, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
                key.SetValue("UninstallString", "\"" + Path.Combine(root, App.Uninstaller) + "\"");
            }
        }

        static void TryDelete(string path)
        {
            try { if (File.Exists(path)) { File.Delete(path); } }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        static void TryDeleteFolder(string path)
        {
            try { if (Directory.Exists(path)) { Directory.Delete(path, true); } }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        public static bool IsElevated()
        {
            using (var identity = WindowsIdentity.GetCurrent())
            {
                return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
            }
        }

        /// Whether files can be written in `folder` (or where it would be made)
        public static bool CanWrite(string folder)
        {
            try
            {
                string existing = Path.GetFullPath(folder);
                while (!Directory.Exists(existing))
                {
                    var parent = Path.GetDirectoryName(existing);
                    if (parent == null) { return false; }
                    existing = parent;
                }
                string probe = Path.Combine(existing, ".jilchat-setup-" + Guid.NewGuid().ToString("N"));
                File.WriteAllText(probe, "");
                File.Delete(probe);
                return true;
            }
            catch (UnauthorizedAccessException) { return false; }
            catch (IOException) { return false; }
            catch (ArgumentException) { return false; }
            catch (NotSupportedException) { return false; }
        }
    }

    /// Shortcuts with the app's AppUserModelID, so the taskbar groups its
    /// windows with the pinned shortcut and notifications show its name.
    static class Shortcut
    {
        [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
        class CShellLink { }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
        interface IShellLinkW
        {
            void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszFile, int cch, IntPtr pfd, uint fFlags);
            void GetIDList(out IntPtr ppidl);
            void SetIDList(IntPtr pidl);
            void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszName, int cch);
            void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string pszName);
            void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszDir, int cch);
            void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string pszDir);
            void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszArgs, int cch);
            void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string pszArgs);
            void GetHotkey(out short pwHotkey);
            void SetHotkey(short wHotkey);
            void GetShowCmd(out int piShowCmd);
            void SetShowCmd(int iShowCmd);
            void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszIconPath, int cch, out int piIcon);
            void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string pszIconPath, int iIcon);
            void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string pszPathRel, uint dwReserved);
            void Resolve(IntPtr hwnd, uint fFlags);
            void SetPath([MarshalAs(UnmanagedType.LPWStr)] string pszFile);
        }

        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        struct PropertyKey
        {
            public Guid FormatId;
            public int PropertyId;
        }

        [StructLayout(LayoutKind.Explicit)]
        struct PropVariant
        {
            [FieldOffset(0)] public ushort Type;
            [FieldOffset(8)] public IntPtr Pointer;
            // PROPVARIANT is 24 bytes on 64-bit Windows
            [FieldOffset(16)] public long Reserved;
        }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
        interface IPropertyStore
        {
            void GetCount(out uint cProps);
            void GetAt(uint iProp, out PropertyKey pkey);
            void GetValue(ref PropertyKey key, out PropVariant pv);
            void SetValue(ref PropertyKey key, ref PropVariant pv);
            void Commit();
        }

        public static void Create(string linkPath, string exe, string workingDirectory)
        {
            var link = (IShellLinkW)new CShellLink();
            link.SetPath(exe);
            link.SetWorkingDirectory(workingDirectory);
            link.SetIconLocation(exe, 0);
            link.SetDescription(App.Name);

            var key = new PropertyKey
            {
                FormatId = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"),
                PropertyId = 5, // PKEY_AppUserModel_ID
            };
            var value = new PropVariant { Type = 31 /* VT_LPWSTR */, Pointer = Marshal.StringToCoTaskMemUni(App.AppUserModelId) };
            try
            {
                var store = (IPropertyStore)link;
                store.SetValue(ref key, ref value);
                store.Commit();
            }
            finally
            {
                Marshal.FreeCoTaskMem(value.Pointer);
            }

            Directory.CreateDirectory(Path.GetDirectoryName(linkPath));
            ((IPersistFile)link).Save(linkPath, true);
            Marshal.ReleaseComObject(link);
        }
    }

    class Spark
    {
        public float X, Y, Speed, Size, Drift, Phase;
    }

    class Hotspot
    {
        public RectangleF Rect;
        public Action Click;
    }

    class SetupWindow : Form
    {
        // Logical size. Everything is drawn in these units and scaled for the display.
        const int ShadowSize = 26;
        const int CardW = 520, CardH = 360;
        const int WinW = CardW + ShadowSize * 2, WinH = CardH + ShadowSize * 2;
        const float Radius = 16f;

        // Black and white
        static readonly Color Ink = Color.FromArgb(9, 9, 10);
        static readonly Color InkLight = Color.FromArgb(28, 28, 31);
        static readonly Color White = Color.FromArgb(250, 250, 250);
        static readonly Color Silver = Color.FromArgb(196, 196, 200);
        static readonly Color Graphite = Color.FromArgb(92, 92, 98);
        static readonly Color Text1 = Color.FromArgb(244, 244, 246);
        static readonly Color Text2 = Color.FromArgb(150, 150, 156);

        readonly Options options;
        readonly float scale;
        readonly Bitmap logo;
        readonly List<Spark> sparks = new List<Spark>();
        readonly Random random = new Random(7);
        readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
        readonly Stopwatch clock = Stopwatch.StartNew();
        readonly List<Hotspot> hotspots = new List<Hotspot>();

        Screen screen = Screen.Welcome;
        Installer installer;
        /// Set while an elevated copy installs for us
        Process elevatedCopy;
        bool desktopShortcut;
        string problem = "";
        double shownProgress;
        double screenStart;
        double doneAt = -1;
        double fadeOutStart = -1;
        double last;
        PointF mouse = new PointF(-1, -1);
        bool mouseDown;
        Point dragOrigin;
        bool dragging;
        Font fontTitle, fontHeading, fontBody, fontSmall, fontButton, fontPercent;

        public SetupWindow(Options options)
        {
            this.options = options;
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            Text = App.Name + " Setup";
            ShowInTaskbar = true;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (ArgumentException) { }

            using (var g = Graphics.FromHwnd(IntPtr.Zero)) { scale = g.DpiX / 96f; }
            Size = new Size((int)(WinW * scale), (int)(WinH * scale));
            var area = System.Windows.Forms.Screen.FromPoint(Cursor.Position).WorkingArea;
            Location = new Point(area.Left + (area.Width - Width) / 2, area.Top + (area.Height - Height) / 2);

            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("logo.png"))
            {
                logo = new Bitmap(stream);
            }

            string family = "Segoe UI";
            fontTitle = new Font(family + " Semibold", 26f, FontStyle.Regular, GraphicsUnit.Pixel);
            fontHeading = new Font(family + " Semibold", 19f, FontStyle.Regular, GraphicsUnit.Pixel);
            fontBody = new Font(family, 13.5f, FontStyle.Regular, GraphicsUnit.Pixel);
            fontSmall = new Font(family, 11.5f, FontStyle.Regular, GraphicsUnit.Pixel);
            fontButton = new Font(family + " Semibold", 15f, FontStyle.Regular, GraphicsUnit.Pixel);
            fontPercent = new Font(family + " Light", 44f, FontStyle.Regular, GraphicsUnit.Pixel);

            // On an update the choice made before stays
            desktopShortcut = options.Desktop.HasValue
                ? options.Desktop.Value
                : File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), App.Name + ".lnk")) ||
                  File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory), App.Name + ".lnk")) ||
                  !options.Update;

            for (int i = 0; i < 34; i++) { sparks.Add(NewSpark(true)); }

            timer.Interval = 16;
            timer.Tick += delegate { Tick(); };
            Shown += delegate
            {
                timer.Start();
                if (options.Auto) { BeginInstall(); }
            };
        }

        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ExStyle |= 0x00080000; // WS_EX_LAYERED
                return cp;
            }
        }

        /// Set while saving pictures of the screens, to show them settled
        double? frozenNow;

        double Now { get { return frozenNow ?? clock.Elapsed.TotalSeconds; } }

        /// Saves each screen, as it looks once its animations are done
        public void SaveScreenshots(string folder)
        {
            Directory.CreateDirectory(folder);
            var shots = new[]
            {
                new KeyValuePair<Screen, string>(Screen.Welcome, "1-welcome.png"),
                new KeyValuePair<Screen, string>(Screen.Installing, "2-installing.png"),
                new KeyValuePair<Screen, string>(Screen.Done, "3-done.png"),
                new KeyValuePair<Screen, string>(Screen.Problem, "4-problem.png"),
            };
            foreach (var shot in shots)
            {
                screen = shot.Key;
                screenStart = 0;
                frozenNow = 2.0;
                shownProgress = shot.Key == Screen.Installing ? 0.62 : 1;
                problem = "Windows blocked access to the install folder. Choose another folder or try again.";
                using (var surface = new Bitmap(Width, Height, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(surface))
                    {
                        g.SmoothingMode = SmoothingMode.AntiAlias;
                        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                        g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                        g.ScaleTransform(scale, scale);
                        Draw(g);
                    }
                    surface.Save(Path.Combine(folder, shot.Value), ImageFormat.Png);
                }
            }
        }

        Spark NewSpark(bool anywhere)
        {
            var e = new Spark();
            e.X = (float)random.NextDouble() * CardW;
            e.Y = anywhere ? (float)random.NextDouble() * CardH : CardH + 6;
            e.Speed = 7f + (float)random.NextDouble() * 20f;
            e.Size = 1f + (float)random.NextDouble() * 2.4f;
            e.Drift = 5f + (float)random.NextDouble() * 12f;
            e.Phase = (float)random.NextDouble() * 6.28f;
            return e;
        }

        void Go(Screen next)
        {
            screen = next;
            screenStart = Now;
        }

        void BeginInstall()
        {
            // The app is closing itself to update: give it time to finish
            if (options.Update)
            {
                for (int waited = 0; waited < 60 && IsRunningFromDestination(); waited++)
                {
                    Thread.Sleep(500);
                }
            }

            if (IsRunningFromDestination())
            {
                problem = App.Name + " is still open. Right-click its icon near the clock, choose Exit, then try again.";
                Go(Screen.Problem);
                return;
            }

            shownProgress = 0;
            doneAt = -1;

            // Program Files and the like: a second copy installs as administrator
            if (!Installer.CanWrite(options.Destination) && !options.Elevated)
            {
                StartElevated();
                return;
            }

            installer = new Installer(options, desktopShortcut);
            installer.Start();
            Go(Screen.Installing);
        }

        void StartElevated()
        {
            try
            {
                var start = new ProcessStartInfo(Application.ExecutablePath, options.ToArguments(desktopShortcut));
                start.UseShellExecute = true;
                start.Verb = "runas";
                // It shows its own progress, this window hides until it's done
                elevatedCopy = Process.Start(start);
                Go(Screen.Installing);
            }
            catch (System.ComponentModel.Win32Exception)
            {
                problem = "This folder needs administrator rights. Allow it when Windows asks, or choose another folder.";
                Go(Screen.Problem);
            }
        }

        bool IsRunningFromDestination()
        {
            string root = Path.GetFullPath(options.Destination).TrimEnd('\\') + "\\";
            foreach (var p in System.Diagnostics.Process.GetProcessesByName(App.Process))
            {
                try
                {
                    if (p.MainModule.FileName.StartsWith(root, StringComparison.OrdinalIgnoreCase)) { return true; }
                }
                catch (System.ComponentModel.Win32Exception) { }
                catch (InvalidOperationException) { }
            }
            return false;
        }

        void Tick()
        {
            double now = Now;
            float dt = (float)Math.Min(0.05, now - last);
            last = now;

            foreach (var e in sparks) { e.Y -= e.Speed * dt; }
            for (int i = 0; i < sparks.Count; i++)
            {
                if (sparks[i].Y < -6) { sparks[i] = NewSpark(false); }
            }

            if (screen == Screen.Installing && elevatedCopy != null)
            {
                if (elevatedCopy.HasExited)
                {
                    bool installed = elevatedCopy.ExitCode == 0;
                    elevatedCopy = null;
                    if (installed)
                    {
                        shownProgress = 1;
                        doneAt = now;
                        Go(Screen.Done);
                    }
                    else
                    {
                        problem = "The install as administrator didn't finish.";
                        Go(Screen.Problem);
                    }
                }
            }
            else if (screen == Screen.Installing && installer != null)
            {
                // Eases toward the real number so the bar moves smoothly
                // and never races through too fast to see on a quick PC
                double step = (installer.Progress - shownProgress) * Math.Min(1.0, dt * 9.0);
                shownProgress += Math.Min(step, dt / 1.5);
                if (installer.Finished && shownProgress > 0.985 || installer.Error != null && installer.Finished)
                {
                    if (installer.Error != null)
                    {
                        problem = installer.Error;
                        Go(Screen.Problem);
                    }
                    else
                    {
                        shownProgress = 1;
                        doneAt = now;
                        Go(Screen.Done);
                    }
                }
            }

            if (screen == Screen.Done && fadeOutStart < 0 && now - doneAt > 1.5)
            {
                fadeOutStart = now;
                if (options.Launch) { LaunchApp(); }
            }
            if (fadeOutStart >= 0 && now - fadeOutStart > 0.35)
            {
                timer.Stop();
                Program.ExitCode = 0;
                Close();
                return;
            }

            Render();
        }

        void LaunchApp()
        {
            try
            {
                var start = new ProcessStartInfo(Path.Combine(options.Destination, App.Exe));
                start.WorkingDirectory = options.Destination;
                start.UseShellExecute = true;
                System.Diagnostics.Process.Start(start);
            }
            catch (System.ComponentModel.Win32Exception)
            {
                // Installed fine: the desktop or Start menu shortcut still opens it
            }
        }

        void ChooseFolder()
        {
            using (var dialog = new FolderBrowserDialog())
            {
                dialog.Description = "Choose where to install " + App.Name;
                dialog.ShowNewFolderButton = true;
                string current = options.Destination;
                while (!Directory.Exists(current) && Path.GetDirectoryName(current) != null)
                {
                    current = Path.GetDirectoryName(current);
                }
                dialog.SelectedPath = current;
                if (dialog.ShowDialog(this) != DialogResult.OK) { return; }

                string chosen = dialog.SelectedPath.TrimEnd('\\');
                // A folder of its own, unless the app is already in the one chosen
                if (!File.Exists(Path.Combine(chosen, App.Exe)) &&
                    !Path.GetFileName(chosen).Equals(App.Name, StringComparison.OrdinalIgnoreCase))
                {
                    chosen = Path.Combine(chosen, App.Name);
                }
                options.Destination = chosen;
            }
        }

        // ------------------------------------------------------------ Drawing

        static GraphicsPath Rounded(RectangleF r, float radius)
        {
            var path = new GraphicsPath();
            float d = radius * 2;
            path.AddArc(r.X, r.Y, d, d, 180, 90);
            path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            return path;
        }

        static double EaseOut(double t)
        {
            t = Math.Max(0, Math.Min(1, t));
            return 1 - Math.Pow(1 - t, 3);
        }

        static Color Fade(Color c, double alpha)
        {
            return Color.FromArgb((int)(Math.Max(0, Math.Min(1, alpha)) * c.A), c);
        }

        void Render()
        {
            int w = Width, h = Height;
            using (var surface = new Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(surface))
                {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                    g.ScaleTransform(scale, scale);
                    Draw(g);
                }

                double intro = EaseOut(Now / 0.35);
                double outro = fadeOutStart >= 0 ? 1 - (Now - fadeOutStart) / 0.35 : 1;
                double hidden = elevatedCopy != null ? 0 : 1;
                Present(surface, Math.Max(0, Math.Min(Math.Min(intro, outro), hidden)));
            }
        }

        void Draw(Graphics g)
        {
            hotspots.Clear();
            DrawShadow(g);

            g.TranslateTransform(ShadowSize, ShadowSize);
            var card = new RectangleF(0, 0, CardW, CardH);
            using (var path = Rounded(card, Radius))
            {
                using (var fill = new LinearGradientBrush(card, InkLight, Ink, 90f))
                {
                    g.FillPath(fill, path);
                }
                var state = g.Save();
                g.SetClip(path);
                DrawGlow(g);
                DrawSparks(g);
                g.Restore(state);
                using (var pen = new Pen(Color.FromArgb(46, 255, 255, 255), 1f))
                {
                    g.DrawPath(pen, path);
                }
            }

            switch (screen)
            {
                case Screen.Welcome: DrawWelcome(g); break;
                case Screen.Installing: DrawInstalling(g); break;
                case Screen.Done: DrawDone(g); break;
                case Screen.Problem: DrawProblem(g); break;
            }

            if (screen != Screen.Installing && screen != Screen.Done) { DrawClose(g); }
            g.TranslateTransform(-ShadowSize, -ShadowSize);
        }

        void DrawShadow(Graphics g)
        {
            for (int i = ShadowSize; i > 0; i -= 2)
            {
                var r = new RectangleF(ShadowSize - i, ShadowSize - i + 4, CardW + i * 2, CardH + i * 2 - 4);
                using (var path = Rounded(r, Radius + i))
                using (var brush = new SolidBrush(Color.FromArgb(6, 0, 0, 0)))
                {
                    g.FillPath(brush, path);
                }
            }
        }

        void DrawGlow(Graphics g)
        {
            double pulse = 0.5 + 0.5 * Math.Sin(Now * 1.3);
            float cx = CardW / 2f, cy = 92f, rx = 280f, ry = 170f;
            using (var path = new GraphicsPath())
            {
                path.AddEllipse(cx - rx, cy - ry, rx * 2, ry * 2);
                using (var brush = new PathGradientBrush(path))
                {
                    brush.CenterColor = Color.FromArgb((int)(30 + 16 * pulse), 255, 255, 255);
                    brush.SurroundColors = new[] { Color.FromArgb(0, 255, 255, 255) };
                    g.FillPath(brush, path);
                }
            }
        }

        void DrawSparks(Graphics g)
        {
            foreach (var e in sparks)
            {
                float x = e.X + (float)Math.Sin(Now * 0.8 + e.Phase) * e.Drift;
                double life = Math.Max(0, Math.Min(1, e.Y / CardH));
                int alpha = (int)(30 + 120 * life * (0.6 + 0.4 * Math.Sin(Now * 2.2 + e.Phase)));
                using (var brush = new SolidBrush(Color.FromArgb(Math.Max(0, Math.Min(255, alpha)), 255, 255, 255)))
                {
                    g.FillEllipse(brush, x, e.Y, e.Size, e.Size);
                }
            }
        }

        /// Draws the logo with a darker band sweeping over it.
        void DrawLogo(Graphics g, float cx, float cy, float side, double alpha)
        {
            int px = Math.Max(8, (int)(side * scale));
            using (var layer = new Bitmap(px, px, System.Drawing.Imaging.PixelFormat.Format32bppPArgb))
            {
                using (var lg = Graphics.FromImage(layer))
                {
                    lg.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    lg.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    lg.SmoothingMode = SmoothingMode.AntiAlias;
                    lg.DrawImage(logo, 0, 0, px, px);
                }
                ApplySweep(layer, (Now % 2.4) / 2.4);

                using (var attributes = new ImageAttributes())
                {
                    var matrix = new ColorMatrix();
                    matrix.Matrix33 = (float)alpha;
                    attributes.SetColorMatrix(matrix);
                    g.DrawImage(layer,
                        new[]
                        {
                            new PointF(cx - side / 2, cy - side / 2),
                            new PointF(cx + side / 2, cy - side / 2),
                            new PointF(cx - side / 2, cy + side / 2)
                        },
                        new RectangleF(0, 0, px, px), GraphicsUnit.Pixel, attributes);
                }
            }
        }

        /// Darkens a moving diagonal band, only where the logo has pixels.
        static void ApplySweep(Bitmap bitmap, double progress)
        {
            var rect = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
            var data = bitmap.LockBits(rect, ImageLockMode.ReadWrite, System.Drawing.Imaging.PixelFormat.Format32bppPArgb);
            try
            {
                int w = bitmap.Width, h = bitmap.Height;
                double center = -0.6 * w + progress * 2.2 * w;
                double band = 0.42 * w;
                var row = new byte[w * 4];
                for (int y = 0; y < h; y++)
                {
                    IntPtr line = new IntPtr(data.Scan0.ToInt64() + (long)y * data.Stride);
                    Marshal.Copy(line, row, 0, row.Length);
                    for (int x = 0; x < w; x++)
                    {
                        double distance = Math.Abs((x + y * 0.4) - center) / band;
                        if (distance >= 1) { continue; }
                        double keep = 1 - 0.55 * (1 - distance) * (1 - distance);
                        int i = x * 4;
                        row[i] = (byte)(row[i] * keep);
                        row[i + 1] = (byte)(row[i + 1] * keep);
                        row[i + 2] = (byte)(row[i + 2] * keep);
                    }
                    Marshal.Copy(row, 0, line, row.Length);
                }
            }
            finally
            {
                bitmap.UnlockBits(data);
            }
        }

        void Centered(Graphics g, string text, Font font, Color color, float y)
        {
            using (var brush = new SolidBrush(color))
            using (var format = new StringFormat())
            {
                format.Alignment = StringAlignment.Center;
                g.DrawString(text, font, brush, new RectangleF(18, y, CardW - 36, 400), format);
            }
        }

        void DrawWelcome(Graphics g)
        {
            double t = Now - screenStart;
            double rise = EaseOut(t / 0.8);
            DrawLogo(g, CardW / 2f, 88 + (float)((1 - rise) * 70), 104, rise);
            double text = EaseOut((t - 0.25) / 0.5);
            Centered(g, App.Name, fontTitle, Fade(Text1, text), 146);
            Centered(g, "Ready to install on this PC", fontBody, Fade(Text2, text), 183);

            double controls = EaseOut((t - 0.4) / 0.5);
            DrawFolder(g, controls);
            DrawCheckbox(g, controls);
            DrawButton(g, new RectangleF(CardW / 2f - 112, 288, 224, 46), "Install", controls, delegate { BeginInstall(); });
        }

        /// The install folder, with a button to choose another
        void DrawFolder(Graphics g, double alpha)
        {
            var r = new RectangleF(60, 214, CardW - 120, 32);
            using (var path = Rounded(r, 8))
            {
                using (var fill = new SolidBrush(Fade(Color.FromArgb(14, 255, 255, 255), alpha))) { g.FillPath(fill, path); }
                using (var pen = new Pen(Fade(Color.FromArgb(40, 255, 255, 255), alpha), 1f)) { g.DrawPath(pen, path); }
            }

            const float buttonW = 74;
            var change = new RectangleF(r.Right - buttonW - 4, r.Y + 4, buttonW, r.Height - 8);
            bool hover = change.Contains(mouse);
            using (var path = Rounded(change, 6))
            using (var fill = new SolidBrush(Fade(Color.FromArgb(hover ? 52 : 26, 255, 255, 255), alpha)))
            {
                g.FillPath(fill, path);
            }

            using (var format = new StringFormat())
            {
                format.LineAlignment = StringAlignment.Center;
                format.Trimming = StringTrimming.EllipsisPath;
                format.FormatFlags = StringFormatFlags.NoWrap;
                using (var brush = new SolidBrush(Fade(Text2, alpha)))
                {
                    g.DrawString(options.Destination, fontSmall, brush,
                        new RectangleF(r.X + 12, r.Y, r.Width - buttonW - 24, r.Height), format);
                }
                format.Alignment = StringAlignment.Center;
                using (var brush = new SolidBrush(Fade(hover ? Text1 : Silver, alpha)))
                {
                    g.DrawString("Change", fontSmall, brush, change, format);
                }
            }
            hotspots.Add(new Hotspot { Rect = change, Click = delegate { ChooseFolder(); } });
        }

        void DrawCheckbox(Graphics g, double alpha)
        {
            string label = "Create a desktop shortcut";
            var size = g.MeasureString(label, fontBody);
            float total = 16 + 8 + size.Width;
            float x = (CardW - total) / 2;
            float y = 258;
            var hit = new RectangleF(x - 6, y - 5, total + 12, 27);
            bool hover = hit.Contains(mouse);
            var box = new RectangleF(x, y, 16, 16);
            using (var path = Rounded(box, 4))
            {
                if (desktopShortcut)
                {
                    using (var fill = new SolidBrush(Fade(White, alpha))) { g.FillPath(fill, path); }
                    using (var pen = new Pen(Fade(Ink, alpha), 1.8f))
                    {
                        pen.StartCap = LineCap.Round;
                        pen.EndCap = LineCap.Round;
                        g.DrawLines(pen, new[] { new PointF(x + 3.6f, y + 8.2f), new PointF(x + 6.8f, y + 11.2f), new PointF(x + 12.4f, y + 5f) });
                    }
                }
                else
                {
                    using (var pen = new Pen(Fade(hover ? Text1 : Text2, alpha), 1.3f)) { g.DrawPath(pen, path); }
                }
            }
            using (var brush = new SolidBrush(Fade(hover ? Text1 : Text2, alpha)))
            {
                g.DrawString(label, fontBody, brush, x + 22, y - 2.5f);
            }
            hotspots.Add(new Hotspot { Rect = hit, Click = delegate { desktopShortcut = !desktopShortcut; } });
        }

        void DrawButton(Graphics g, RectangleF r, string label, double alpha, Action click)
        {
            bool hover = r.Contains(mouse);
            bool pressed = hover && mouseDown;
            if (pressed) { r.Inflate(-1.5f, -1); }

            if (hover)
            {
                for (int i = 10; i > 0; i -= 2)
                {
                    var glow = r;
                    glow.Inflate(i, i);
                    using (var path = Rounded(glow, 12 + i))
                    using (var brush = new SolidBrush(Fade(Color.FromArgb(12, 255, 255, 255), alpha)))
                    {
                        g.FillPath(brush, path);
                    }
                }
            }

            using (var path = Rounded(r, 11))
            {
                using (var fill = new LinearGradientBrush(r, Fade(hover ? White : Text1, alpha), Fade(Silver, alpha), 90f))
                {
                    g.FillPath(fill, path);
                }
                var state = g.Save();
                g.SetClip(path);
                double phase = (Now % 3.2) / 3.2;
                float sx = r.X - 80 + (float)(phase * (r.Width + 160));
                var band = new RectangleF(sx, r.Y, 80, r.Height);
                using (var shine = new LinearGradientBrush(band, Color.FromArgb(0, 255, 255, 255), Fade(Color.FromArgb(90, 255, 255, 255), alpha), 0f))
                {
                    var blend = new ColorBlend(3);
                    blend.Colors = new[] { Color.FromArgb(0, 255, 255, 255), Fade(Color.FromArgb(90, 255, 255, 255), alpha), Color.FromArgb(0, 255, 255, 255) };
                    blend.Positions = new[] { 0f, 0.5f, 1f };
                    shine.InterpolationColors = blend;
                    g.FillRectangle(shine, band);
                }
                g.Restore(state);
                using (var pen = new Pen(Fade(Color.FromArgb(120, 255, 255, 255), alpha), 1f)) { g.DrawPath(pen, path); }
            }
            using (var brush = new SolidBrush(Fade(Ink, alpha)))
            using (var format = new StringFormat())
            {
                format.Alignment = StringAlignment.Center;
                format.LineAlignment = StringAlignment.Center;
                g.DrawString(label, fontButton, brush, r, format);
            }
            hotspots.Add(new Hotspot { Rect = r, Click = click });
        }

        void DrawInstalling(Graphics g)
        {
            double pulse = 1 + 0.025 * Math.Sin(Now * 3);
            DrawLogo(g, CardW / 2f, 82, (float)(84 * pulse), 1);
            Centered(g, options.Update ? "Updating " + App.Name : "Installing " + App.Name, fontHeading, Text1, 136);
            Centered(g, (int)Math.Round(shownProgress * 100) + "%", fontPercent, Text1, 166);
            DrawProgress(g, new RectangleF(44, 250, CardW - 88, 12), shownProgress);

            string file = installer != null ? installer.CurrentFile : "";
            if (file.Length > 62) { file = "..." + file.Substring(file.Length - 59); }
            Centered(g, file, fontSmall, Text2, 274);
            Centered(g, "This only takes a moment", fontSmall, Fade(Text2, 0.7), 314);
        }

        void DrawProgress(Graphics g, RectangleF track, double value)
        {
            using (var path = Rounded(track, track.Height / 2))
            using (var brush = new SolidBrush(Color.FromArgb(150, 0, 0, 0)))
            {
                g.FillPath(brush, path);
            }

            float width = Math.Max(track.Height, (float)(track.Width * value));
            var bar = new RectangleF(track.X, track.Y, width, track.Height);
            for (int i = 8; i > 0; i -= 2)
            {
                var glow = bar;
                glow.Inflate(i, i);
                using (var path = Rounded(glow, track.Height / 2 + i))
                using (var brush = new SolidBrush(Color.FromArgb(10, 255, 255, 255)))
                {
                    g.FillPath(brush, path);
                }
            }
            using (var path = Rounded(bar, track.Height / 2))
            {
                var wide = new RectangleF(track.X, track.Y, track.Width + 1, track.Height);
                using (var fill = new LinearGradientBrush(wide, Graphite, White, 0f))
                {
                    g.FillPath(fill, path);
                }
                var state = g.Save();
                g.SetClip(path);
                double phase = (Now % 1.5) / 1.5;
                float sx = bar.X - 90 + (float)(phase * (bar.Width + 180));
                var band = new RectangleF(sx, bar.Y, 90, bar.Height);
                using (var shine = new LinearGradientBrush(band, Color.Transparent, Color.Transparent, 0f))
                {
                    var blend = new ColorBlend(3);
                    blend.Colors = new[] { Color.FromArgb(0, 255, 255, 255), Color.FromArgb(140, 255, 255, 255), Color.FromArgb(0, 255, 255, 255) };
                    blend.Positions = new[] { 0f, 0.5f, 1f };
                    shine.InterpolationColors = blend;
                    g.FillRectangle(shine, band);
                }
                g.Restore(state);
            }
        }

        void DrawDone(Graphics g)
        {
            double t = EaseOut((Now - screenStart) / 0.45);
            DrawLogo(g, CardW / 2f, 112, (float)(104 + 16 * t), 1);
            Centered(g, "You're all set", fontTitle, Fade(Text1, t), 190);
            Centered(g, options.Launch ? "Opening " + App.Name + "..." : App.Name + " is installed", fontBody, Fade(Text2, t), 230);
            DrawProgress(g, new RectangleF(44, 286, CardW - 88, 8), 1);
        }

        void DrawProblem(Graphics g)
        {
            double t = EaseOut((Now - screenStart) / 0.35);
            DrawLogo(g, CardW / 2f, 72, 72, t);
            Centered(g, "That didn't finish", fontHeading, Fade(Text1, t), 118);
            using (var brush = new SolidBrush(Fade(Text2, t)))
            using (var format = new StringFormat())
            {
                format.Alignment = StringAlignment.Center;
                format.Trimming = StringTrimming.EllipsisWord;
                g.DrawString(problem, fontBody, brush, new RectangleF(44, 152, CardW - 88, 110), format);
            }
            DrawButton(g, new RectangleF(CardW / 2f - 92, 282, 184, 44), "Try again", t, delegate { BeginInstall(); });
        }

        void DrawClose(Graphics g)
        {
            var r = new RectangleF(CardW - 42, 12, 30, 30);
            bool hover = r.Contains(mouse);
            if (hover)
            {
                using (var path = Rounded(r, 8))
                using (var brush = new SolidBrush(Color.FromArgb(50, 255, 255, 255)))
                {
                    g.FillPath(brush, path);
                }
            }
            using (var pen = new Pen(hover ? Text1 : Text2, 1.6f))
            {
                pen.StartCap = LineCap.Round;
                pen.EndCap = LineCap.Round;
                g.DrawLine(pen, r.X + 10, r.Y + 10, r.Right - 10, r.Bottom - 10);
                g.DrawLine(pen, r.Right - 10, r.Y + 10, r.X + 10, r.Bottom - 10);
            }
            hotspots.Add(new Hotspot { Rect = r, Click = delegate { timer.Stop(); Close(); } });
        }

        // ------------------------------------------------------------ Mouse

        PointF ToCard(Point p)
        {
            return new PointF(p.X / scale - ShadowSize, p.Y / scale - ShadowSize);
        }

        Hotspot HotspotAt(PointF p)
        {
            for (int i = hotspots.Count - 1; i >= 0; i--)
            {
                if (hotspots[i].Rect.Contains(p)) { return hotspots[i]; }
            }
            return null;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            mouse = ToCard(e.Location);
            if (dragging)
            {
                Location = new Point(Cursor.Position.X - dragOrigin.X, Cursor.Position.Y - dragOrigin.Y);
                return;
            }
            Cursor = HotspotAt(mouse) != null ? Cursors.Hand : Cursors.Default;
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            base.OnMouseLeave(e);
            mouse = new PointF(-1, -1);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left) { return; }
            mouseDown = true;
            if (HotspotAt(ToCard(e.Location)) == null)
            {
                dragging = true;
                dragOrigin = new Point(Cursor.Position.X - Left, Cursor.Position.Y - Top);
            }
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (e.Button != MouseButtons.Left) { return; }
            mouseDown = false;
            if (dragging) { dragging = false; return; }
            var spot = HotspotAt(ToCard(e.Location));
            if (spot != null) { spot.Click(); }
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if (screen == Screen.Welcome || screen == Screen.Problem)
            {
                if (e.KeyCode == Keys.Enter) { BeginInstall(); }
                else if (e.KeyCode == Keys.Escape) { timer.Stop(); Close(); }
            }
        }

        // ------------------------------------------------------------ Window

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            // Stopping halfway through would leave a broken install
            if (screen == Screen.Installing && e.CloseReason == CloseReason.UserClosing)
            {
                e.Cancel = true;
                return;
            }
            base.OnFormClosing(e);
        }

        void Present(Bitmap bitmap, double opacity)
        {
            IntPtr screenDc = Native.GetDC(IntPtr.Zero);
            IntPtr memDc = Native.CreateCompatibleDC(screenDc);
            IntPtr hBitmap = bitmap.GetHbitmap(Color.FromArgb(0));
            IntPtr old = Native.SelectObject(memDc, hBitmap);
            try
            {
                var size = new Native.SIZE { cx = bitmap.Width, cy = bitmap.Height };
                var source = new Native.POINT();
                var top = new Native.POINT { x = Left, y = Top };
                var blend = new Native.BLENDFUNCTION
                {
                    BlendOp = 0,
                    BlendFlags = 0,
                    SourceConstantAlpha = (byte)(255 * opacity),
                    AlphaFormat = 1
                };
                Native.UpdateLayeredWindow(Handle, screenDc, ref top, ref size, memDc, ref source, 0, ref blend, 2);
            }
            finally
            {
                Native.SelectObject(memDc, old);
                Native.DeleteObject(hBitmap);
                Native.DeleteDC(memDc);
                Native.ReleaseDC(IntPtr.Zero, screenDc);
            }
        }
    }

    static class Native
    {
        [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
        [StructLayout(LayoutKind.Sequential)] public struct SIZE { public int cx, cy; }
        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct BLENDFUNCTION { public byte BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat; }

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool UpdateLayeredWindow(IntPtr hwnd, IntPtr hdcDst, ref POINT pptDst, ref SIZE psize, IntPtr hdcSrc, ref POINT pptSrc, int crKey, ref BLENDFUNCTION pblend, int dwFlags);
        [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);
        [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr hdc);
        [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr hdc);
        [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr hdc, IntPtr obj);
        [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr obj);
    }

    static class Program
    {
        /// 0 once installed; an elevated copy reports it this way
        public static int ExitCode = 1;

        [STAThread]
        static int Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            var options = Options.Parse(args);
            var window = new SetupWindow(options);
            if (options.ScreenshotFolder != null)
            {
                window.SaveScreenshots(options.ScreenshotFolder);
                return 0;
            }
            Application.Run(window);
            return ExitCode;
        }
    }
}
