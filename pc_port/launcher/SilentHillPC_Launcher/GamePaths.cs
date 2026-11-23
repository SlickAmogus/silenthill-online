using System;
using System.IO;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// The online build's file names. They differ from the single-player build's
    /// (SilentHillPC.exe, SilentHillPC_Launcher.exe, config.cfg, maps/) so the two
    /// builds can share one folder.
    /// </summary>
    public static class GamePaths
    {
        public const string GameExe          = "SilentHillOnline.exe";
        /// <summary>What online builds were called before. Started only when the new
        /// name is missing, after an update that did not finish.</summary>
        public const string LegacyGameExe    = "SilentHillPC.exe";
        public const string ConfigFile       = "onlineconfig.cfg";
        public const string LegacyConfigFile = "config.cfg";

        public static string Dir { get { return AppDomain.CurrentDomain.BaseDirectory; } }

        /// <summary>The game exe in <paramref name="dir"/> to start: the new name, else
        /// the old one; the new name when neither is there.</summary>
        public static string GameExePath(string dir)
        {
            string exe = Path.Combine(dir, GameExe);
            if (File.Exists(exe)) return exe;
            string legacy = Path.Combine(dir, LegacyGameExe);
            return File.Exists(legacy) ? legacy : exe;
        }

        public static bool HasGameExe(string dir)
        {
            return File.Exists(GameExePath(dir));
        }

        /// <summary>onlineconfig.cfg in <paramref name="dir"/>. The first time, when only
        /// config.cfg is there (an older online build's settings, or the single-player
        /// build's in a shared folder), it starts as a copy of that, as the game does.</summary>
        public static string ConfigPath(string dir)
        {
            string cfg = Path.Combine(dir, ConfigFile);
            try
            {
                string legacy = Path.Combine(dir, LegacyConfigFile);
                if (!File.Exists(cfg) && File.Exists(legacy))
                    File.Copy(legacy, cfg, false);
            }
            catch { /* read-only folder: the launcher works off defaults, as it always has */ }
            return cfg;
        }
    }
}
