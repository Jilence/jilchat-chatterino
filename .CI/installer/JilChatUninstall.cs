// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

// JilChat Chatterino's uninstaller, after the one of Solrino (MIT). The setup
// puts it in the install folder as "Uninstall JilChat Chatterino.exe" and
// points Installed apps at it. A program instead of a PowerShell script, which
// antivirus apps find suspicious.
//
// It removes the program files, shortcuts and the Installed apps entry. The
// settings in %APPDATA%\Chatterino2 are kept, so a reinstall picks them up.
//
// A running program can't delete itself, so after the questions it copies
// itself to the temp folder and that copy finishes the job once this one
// exits. An install for all users (in Program Files) asks for administrator
// rights first.
//
// Built by build-installer.ps1 with the C# compiler that ships with Windows,
// so it's C# 5.

using System;
using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace JilChatUninstall
{
    static class Program
    {
        const string Title = "JilChat Chatterino";
        const string Exe = "JilChatChatterino.exe";
        const string ProcessName = "JilChatChatterino";
        const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\JilChatChatterino";

        [STAThread]
        static int Main(string[] args)
        {
            Application.EnableVisualStyles();
            if (args.Length == 3 && args[0] == "--finish")
            {
                return Finish(args[1], args[2]);
            }
            return Start(args.Length == 1 && args[0] == "--confirmed");
        }

        static int Start(bool confirmed)
        {
            string self = Process.GetCurrentProcess().MainModule.FileName;
            string folder = Path.GetDirectoryName(self);

            if (!File.Exists(Path.Combine(folder, Exe)))
            {
                MessageBox.Show(Title + " was not found in this folder.", Title);
                return 1;
            }

            if (!confirmed)
            {
                var answer = MessageBox.Show(
                    "Remove " + Title + " from this computer?\n\nYour settings stay in %APPDATA%\\Chatterino2.",
                    Title, MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                if (answer != DialogResult.Yes) { return 0; }
            }

            // Installed for all users: that needs an administrator
            if (!IsElevated() && (IsMachineInstall() || !CanWrite(folder)))
            {
                try
                {
                    var start = new ProcessStartInfo(self, "--confirmed");
                    start.UseShellExecute = true;
                    start.Verb = "runas";
                    Process.Start(start);
                }
                catch (System.ComponentModel.Win32Exception)
                {
                    MessageBox.Show("Removing " + Title + " needs administrator rights.", Title);
                    return 1;
                }
                return 0;
            }

            if (IsRunningFrom(folder))
            {
                MessageBox.Show("Exit " + Title + " from its tray icon first, then try again.", Title);
                return 1;
            }

            foreach (var place in new[] {
                Environment.SpecialFolder.DesktopDirectory, Environment.SpecialFolder.Programs,
                Environment.SpecialFolder.CommonDesktopDirectory, Environment.SpecialFolder.CommonPrograms })
            {
                TryDelete(Path.Combine(Environment.GetFolderPath(place), Title + ".lnk"));
            }
            foreach (var hive in new[] { Registry.CurrentUser, Registry.LocalMachine })
            {
                try { hive.DeleteSubKeyTree(UninstallKey, false); }
                catch (Exception) { }
            }

            // Hand the folder over to a copy outside it
            string helper = Path.Combine(Path.GetTempPath(), "JilChat-Uninstall-" + Guid.NewGuid().ToString("N") + ".exe");
            try
            {
                File.Copy(self, helper, true);
                var start = new ProcessStartInfo(helper);
                start.Arguments = "--finish \"" + folder + "\" " + Process.GetCurrentProcess().Id;
                start.UseShellExecute = false;
                Process.Start(start);
            }
            catch (Exception)
            {
                // Couldn't hand off: remove everything but this file
                DeleteFolder(folder, self);
                MessageBox.Show(Title + " was removed.", Title);
            }
            return 0;
        }

        static int Finish(string folder, string parentId)
        {
            int id;
            if (int.TryParse(parentId, out id))
            {
                try { Process.GetProcessById(id).WaitForExit(15000); }
                catch (ArgumentException) { }
            }

            // Only ever a folder that holds the app, never anything else
            if (Directory.Exists(folder) && File.Exists(Path.Combine(folder, Exe)))
            {
                for (int attempt = 0; attempt < 10 && Directory.Exists(folder); attempt++)
                {
                    DeleteFolder(folder, null);
                    if (Directory.Exists(folder)) { Thread.Sleep(500); }
                }
            }
            MessageBox.Show(Title + " was removed.", Title);

            // This copy is in the temp folder; Windows removes it with the rest
            return 0;
        }

        static bool IsElevated()
        {
            using (var identity = WindowsIdentity.GetCurrent())
            {
                return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
            }
        }

        static bool IsMachineInstall()
        {
            try
            {
                using (var key = Registry.LocalMachine.OpenSubKey(UninstallKey))
                {
                    return key != null;
                }
            }
            catch (Exception) { return false; }
        }

        static bool CanWrite(string folder)
        {
            try
            {
                string probe = Path.Combine(folder, ".jilchat-uninstall-" + Guid.NewGuid().ToString("N"));
                File.WriteAllText(probe, "");
                File.Delete(probe);
                return true;
            }
            catch (Exception) { return false; }
        }

        static bool IsRunningFrom(string folder)
        {
            string prefix = folder.TrimEnd('\\') + "\\";
            foreach (var process in Process.GetProcessesByName(ProcessName))
            {
                try
                {
                    if (process.MainModule.FileName.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    {
                        return true;
                    }
                }
                catch (Exception) { }
            }
            return false;
        }

        static void DeleteFolder(string folder, string keep)
        {
            if (keep == null)
            {
                try { Directory.Delete(folder, true); } catch (Exception) { }
                return;
            }
            foreach (var file in Directory.GetFiles(folder, "*", SearchOption.AllDirectories))
            {
                if (!string.Equals(file, keep, StringComparison.OrdinalIgnoreCase)) { TryDelete(file); }
            }
            foreach (var sub in Directory.GetDirectories(folder))
            {
                try { Directory.Delete(sub, true); } catch (Exception) { }
            }
        }

        static void TryDelete(string path)
        {
            try { if (File.Exists(path)) { File.Delete(path); } }
            catch (Exception) { }
        }
    }
}
