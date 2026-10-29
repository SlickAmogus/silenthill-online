using System;
using System.IO;
using System.Threading;
using System.Windows.Forms;
using SilentHillPC_Launcher;

internal static class Program
{
    // Held for the process lifetime so the named mutex stays alive while we run;
    // a static field is a GC root, so it won't be collected mid-session.
    static Mutex s_singleInstance;

    [STAThread]
    static void Main()
    {
        bool createdNew;
        /* Its own name, so the Online launcher and the single-player one can be
         * open at the same time; each still keeps to one copy of itself. */
        s_singleInstance = new Mutex(false, @"Local\SilentHillOnline_Launcher_SingleInstance", out createdNew);
        if (!createdNew)
        {
            try
            {
                var cfg = new ConfigManager(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.cfg"));
                Loc.Current = LauncherSettings.Load(cfg).Language;
            }
            catch { }
            MessageBox.Show(
                Loc.T("Silent Hill Online Launcher is already running."),
                "Silent Hill Online Launcher",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new Form1());
    }
}
