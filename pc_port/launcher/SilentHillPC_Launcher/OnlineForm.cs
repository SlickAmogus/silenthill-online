using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Silent Hill Online settings.
    ///
    /// Sections, top to bottom: the living world you connect to (server, name),
    /// what of it you see, the three ways of playing co-op (rooms on that
    /// server, a room on this PC over IP, Steam), and hosting a living world
    /// server of your own. Everything is written to the online_* keys in
    /// config.cfg; the game reads them at startup.
    ///
    /// Test connection speaks the real protocol: HELLO, the server's CHALLENGE
    /// cookie echoed back with the password proof and this install's build id,
    /// then WELCOME or REJECT, then BYE. A pass therefore means the server is
    /// up, speaks this version, and admits this build with this password.
    /// </summary>
    public class OnlineForm : Form
    {
        private readonly ConfigManager _config;

        public const string DefaultServer = "server.sh1pc.com";
        public const int DefaultPort = 27888;

        private CheckBox _chkEnable;
        private TextBox _txtServer;
        private NumericUpDown _numPort;
        private TextBox _txtName;
        private TextBox _txtPassword;
        private Button _btnTest;
        private Button _btnReset;
        private Label _lblStatus;

        private CheckBox _chkGhosts;
        private CheckBox _chkMemos;
        private CheckBox _chkDeaths;
        private CheckBox _chkEvents;
        private CheckBox _chkNameplates;
        private ComboBox _cboStyle;
        private TrackBar _trkRange;
        private Label _lblRange;

        private NumericUpDown _numRoomMax;

        private NumericUpDown _numSteamAppId;
        private CheckBox _chkSteamPublic;
        private CheckBox _chkSteamAutoHost;
        private Button _btnSteamCheck;
        private Label _lblSteamState;

        private Button _btnHost;
        private Button _btnHostStop;
        private Label _lblHostState;
        private Button _btnClose;

        private Process _server;

        // Must match sh_net_proto.h. Only the handshake is duplicated here.
        private const int ProtoVer = 1;
        private const int HdrSize = 12;
        private const int MsgHello = 0x01;
        private const int MsgWelcome = 0x02;
        private const int MsgBye = 0x05;
        private const int MsgReject = 0x06;
        private const int MsgChallenge = 0x07;
        private const int NameMax = 24;
        private const int BuildMax = 24;
        private const int ServerMax = 32;
        private const int MotdMax = 128;
        private const int RejectMax = 96;

        public OnlineForm(ConfigManager config)
        {
            _config = config;
            BuildUi();
            LoadFromConfig();
        }

        private static Label Lbl(string text, int x, int y, int w = 120)
        {
            return new Label { Location = new Point(x, y + 3), Size = new Size(w, 15), Text = text };
        }

        private static Label Note(string text, int x, int y, int w, int h)
        {
            return new Label { Location = new Point(x, y), Size = new Size(w, h), Text = text };
        }

        private void BuildUi()
        {
            Text = "Silent Hill Online";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(560, 742);

            // ---- Living world: the server you connect to, and your name ----
            var grpWorld = new GroupBox
            {
                Location = new Point(12, 8),
                Size = new Size(536, 122),
                Text = "Living world"
            };

            _chkEnable = new CheckBox
            {
                Location = new Point(14, 20),
                Size = new Size(380, 20),
                Text = "Play the living world (other players' ghosts and messages)"
            };

            _txtServer = new TextBox { Location = new Point(96, 46), Size = new Size(250, 22) };
            _numPort = new NumericUpDown
            {
                Location = new Point(410, 46),
                Size = new Size(72, 22),
                Minimum = 1,
                Maximum = 65535,
                Value = DefaultPort
            };
            _txtName = new TextBox { Location = new Point(96, 72), Size = new Size(180, 22), MaxLength = 23 };
            _txtPassword = new TextBox
            {
                Location = new Point(410, 72),
                Size = new Size(110, 22),
                UseSystemPasswordChar = true
            };

            _btnTest = new Button { Location = new Point(96, 96), Size = new Size(110, 22), Text = "Test connection" };
            _btnTest.Click += BtnTest_Click;
            _btnReset = new Button { Location = new Point(212, 96), Size = new Size(110, 22), Text = "Reset to default" };
            _btnReset.Click += (s, e) =>
            {
                _txtServer.Text = DefaultServer;
                _numPort.Value = DefaultPort;
                _txtPassword.Text = "";
                _lblStatus.Text = "";
            };
            _lblStatus = new Label { Location = new Point(330, 99), Size = new Size(200, 18), Text = "" };

            grpWorld.Controls.AddRange(new Control[]
            {
                _chkEnable,
                Lbl("Server", 12, 46, 80), _txtServer,
                Lbl("Port", 372, 46, 34), _numPort,
                Lbl("Your name", 12, 72, 80), _txtName,
                Lbl("Password", 348, 72, 58), _txtPassword,
                _btnTest, _btnReset, _lblStatus
            });

            // ---- What you see ----
            var grpWhat = new GroupBox
            {
                Location = new Point(12, 136),
                Size = new Size(536, 124),
                Text = "What you see"
            };

            _chkGhosts = new CheckBox { Location = new Point(14, 20), Size = new Size(220, 20), Text = "Other players, as outlines" };
            _chkMemos = new CheckBox { Location = new Point(14, 40), Size = new Size(220, 20), Text = "Messages people leave" };
            _chkDeaths = new CheckBox { Location = new Point(14, 60), Size = new Size(220, 20), Text = "Markers where people died" };
            _chkEvents = new CheckBox { Location = new Point(14, 80), Size = new Size(220, 20), Text = "Join / leave / death feed" };
            _chkNameplates = new CheckBox { Location = new Point(14, 100), Size = new Size(220, 20), Text = "Names over players in co-op" };

            _cboStyle = new ComboBox
            {
                Location = new Point(348, 20),
                Size = new Size(174, 22),
                DropDownStyle = ComboBoxStyle.DropDownList
            };
            _cboStyle.Items.AddRange(new object[] { "Outline only", "Floor ring only", "Outline and ring" });

            _trkRange = new TrackBar
            {
                Location = new Point(340, 58),
                Size = new Size(190, 40),
                Minimum = 5,
                Maximum = 120,
                TickFrequency = 15,
                Value = 40
            };
            _trkRange.ValueChanged += (s, e) => UpdateRangeLabel();
            _lblRange = new Label { Location = new Point(348, 100), Size = new Size(180, 18), Text = "" };

            grpWhat.Controls.AddRange(new Control[]
            {
                _chkGhosts, _chkMemos, _chkDeaths, _chkEvents, _chkNameplates,
                Lbl("How ghosts draw", 258, 20, 90), _cboStyle,
                Lbl("Visible within", 258, 62, 80), _trkRange, _lblRange
            });

            // ---- Co-op rooms on the server ----
            var grpRooms = new GroupBox
            {
                Location = new Point(12, 266),
                Size = new Size(536, 70),
                Text = "Co-op rooms on the server"
            };
            _numRoomMax = new NumericUpDown
            {
                Location = new Point(430, 40),
                Size = new Size(52, 22),
                Minimum = 2,
                Maximum = 4,
                Value = 4
            };
            grpRooms.Controls.AddRange(new Control[]
            {
                Note("Open a room from the in-game Multiplayer menu: Host Game, Host on: My Server. " +
                     "Friends join from Browse Server Rooms or with the 4-digit room code. No port forwarding.",
                     14, 18, 508, 30),
                Lbl("Players (default)", 328, 40, 100), _numRoomMax
            });

            // ---- Co-op over IP ----
            var grpIp = new GroupBox
            {
                Location = new Point(12, 342),
                Size = new Size(536, 56),
                Text = "Co-op over IP (this PC hosts)"
            };
            grpIp.Controls.Add(Note("Host on: This PC in the in-game menu; friends use Join by IP. " +
                                    "Forward UDP " + DefaultPort + " (the port above) to this PC first.",
                                    14, 18, 508, 30));

            // ---- Co-op over Steam ----
            var grpSteam = new GroupBox
            {
                Location = new Point(12, 404),
                Size = new Size(536, 98),
                Text = "Co-op over Steam"
            };

            _chkSteamPublic = new CheckBox
            {
                Location = new Point(176, 46),
                Size = new Size(110, 20),
                Text = "Public lobby"
            };
            _chkSteamAutoHost = new CheckBox
            {
                Location = new Point(292, 46),
                Size = new Size(150, 20),
                Text = "Open one at startup"
            };
            _numSteamAppId = new NumericUpDown
            {
                Location = new Point(76, 44),
                Size = new Size(84, 22),
                Minimum = 1,
                Maximum = 2000000000,
                Value = 480
            };
            _btnSteamCheck = new Button
            {
                Location = new Point(442, 43),
                Size = new Size(80, 22),
                Text = "Check Steam"
            };
            _btnSteamCheck.Click += BtnSteamCheck_Click;
            _lblSteamState = new Label { Location = new Point(14, 72), Size = new Size(508, 18), Text = "" };

            grpSteam.Controls.AddRange(new Control[]
            {
                Note("Host on: Steam in the in-game menu, then invite from the overlay or the friends list.",
                     14, 18, 508, 18),
                Lbl("App ID", 14, 44, 58), _numSteamAppId,
                _chkSteamPublic, _chkSteamAutoHost, _btnSteamCheck, _lblSteamState
            });

            // ---- Host a living world server ----
            var grpHost = new GroupBox
            {
                Location = new Point(12, 508),
                Size = new Size(536, 92),
                Text = "Host a living world server on this PC"
            };
            _btnHost = new Button { Location = new Point(14, 44), Size = new Size(120, 22), Text = "Start server here" };
            _btnHost.Click += BtnHost_Click;
            _btnHostStop = new Button { Location = new Point(140, 44), Size = new Size(80, 22), Text = "Stop" };
            _btnHostStop.Click += BtnHostStop_Click;
            _lblHostState = new Label { Location = new Point(14, 70), Size = new Size(508, 18), Text = "" };
            grpHost.Controls.AddRange(new Control[]
            {
                Note("Runs online_server\\sh_master.exe on the port and password above. Players reach it at " +
                     "this PC's address once UDP " + DefaultPort + " is forwarded; set Server to 127.0.0.1 to play on it yourself.",
                     14, 18, 508, 28),
                _btnHost, _btnHostStop, _lblHostState
            });

            var keys = new Label
            {
                Location = new Point(14, 610),
                Size = new Size(440, 18),
                Text = "In game:  M  multiplayer menu      B  voice lines      console:  status"
            };

            _btnClose = new Button
            {
                Location = new Point(452, 706),
                Size = new Size(96, 26),
                Text = "Save",
                DialogResult = DialogResult.OK
            };
            _btnClose.Click += (s, e) => SaveToConfig();

            var tip = new ToolTip { AutoPopDelay = 30000, InitialDelay = 400, ReshowDelay = 100 };
            tip.SetToolTip(_txtServer, "The living world server. " + DefaultServer + " is the community one.");
            tip.SetToolTip(_btnTest, "Joins the server for a moment with your name, password and this build, " +
                                     "then leaves: a pass means it will work in game too.");
            tip.SetToolTip(_btnReset, "Back to " + DefaultServer + ":" + DefaultPort + " with no password.");
            tip.SetToolTip(_txtName, "How other players see you: over your head in co-op, on your ghost and " +
                                     "your messages in the living world.");
            tip.SetToolTip(_trkRange, "Metres past which another player's ghost is not drawn.");
            tip.SetToolTip(_chkDeaths, "Your own deaths are reported too, so other players see where you fell.");
            tip.SetToolTip(_numSteamAppId, "480 is Spacewar, Valve's public test app, which every Steam account " +
                                           "owns. Change this only if the project ever gets a real one.");
            tip.SetToolTip(_btnSteamCheck, "Loads steam_api64.dll the way the game does and creates a throwaway " +
                                           "lobby. Names whatever did not work.");
            tip.SetToolTip(_chkSteamPublic, "Off means friends only, which is what an invite-driven session wants.");
            tip.SetToolTip(_btnHost, "Starts sh_master.exe in a console window on this machine.");

            Controls.AddRange(new Control[] { grpWorld, grpWhat, grpRooms, grpIp, grpSteam, grpHost, keys, _btnClose });
            AcceptButton = _btnClose;
        }

        private void UpdateRangeLabel()
        {
            _lblRange.Text = _trkRange.Value + " metres";
        }

        private static int GetInt(ConfigManager c, string key, int def)
        {
            int v;
            return int.TryParse(c.Get(key, def.ToString()), out v) ? v : def;
        }

        private void LoadFromConfig()
        {
            _chkEnable.Checked = GetInt(_config, "online_enabled", 0) != 0;
            _txtServer.Text = _config.Get("online_server", DefaultServer);
            if (_txtServer.Text.Trim().Length == 0) _txtServer.Text = DefaultServer;
            _numPort.Value = Math.Min(65535, Math.Max(1, GetInt(_config, "online_port", DefaultPort)));
            _txtName.Text = _config.Get("online_name", "Wanderer");
            _txtPassword.Text = _config.Get("online_password", "");
            _chkGhosts.Checked = GetInt(_config, "online_ghosts", 1) != 0;
            _chkMemos.Checked = GetInt(_config, "online_memos", 1) != 0;
            _chkDeaths.Checked = GetInt(_config, "online_deaths", 1) != 0;
            _chkEvents.Checked = GetInt(_config, "online_events", 1) != 0;
            _chkNameplates.Checked = GetInt(_config, "online_nameplates", 1) != 0;
            _cboStyle.SelectedIndex = Math.Min(2, Math.Max(0, GetInt(_config, "online_ghost_style", 2)));
            _trkRange.Value = Math.Min(_trkRange.Maximum,
                                       Math.Max(_trkRange.Minimum, GetInt(_config, "online_ghost_range", 40)));
            UpdateRangeLabel();

            _numRoomMax.Value         = Math.Min(4, Math.Max(2, GetInt(_config, "online_steam_max_players", 4)));
            _numSteamAppId.Value      = Math.Max(1, GetInt(_config, "online_steam_appid", 480));
            _chkSteamPublic.Checked   = GetInt(_config, "online_steam_public", 0) != 0;
            _chkSteamAutoHost.Checked = GetInt(_config, "online_steam_autohost", 0) != 0;
            _lblSteamState.Text       = File.Exists(Path.Combine(
                                            AppDomain.CurrentDomain.BaseDirectory, "steam_api64.dll"))
                                      ? "steam_api64.dll found."
                                      : "steam_api64.dll is not next to the game; Steam co-op stays off.";
            UpdateHostState();
        }

        private void SaveToConfig()
        {
            // Sanitised the same way the game sanitises it, so what the
            // launcher shows and what other players see are the same string.
            var name = new StringBuilder();
            foreach (char ch in _txtName.Text)
            {
                if (name.Length >= NameMax - 1) break;
                name.Append(ch >= 32 && ch <= 126 ? ch : '?');
            }
            if (name.ToString().Trim().Length == 0) name = new StringBuilder("Wanderer");

            string server = _txtServer.Text.Trim();
            if (server.Length == 0) server = DefaultServer;

            _config.Set("online_enabled", _chkEnable.Checked ? "1" : "0");
            _config.Set("online_server", server);
            _config.Set("online_port", ((int)_numPort.Value).ToString());
            _config.Set("online_name", name.ToString());
            _config.Set("online_password", _txtPassword.Text);
            _config.Set("online_ghosts", _chkGhosts.Checked ? "1" : "0");
            _config.Set("online_memos", _chkMemos.Checked ? "1" : "0");
            _config.Set("online_deaths", _chkDeaths.Checked ? "1" : "0");
            _config.Set("online_events", _chkEvents.Checked ? "1" : "0");
            _config.Set("online_ghost_style", _cboStyle.SelectedIndex.ToString());
            _config.Set("online_ghost_range", _trkRange.Value.ToString());
            _config.Set("online_nameplates", _chkNameplates.Checked ? "1" : "0");
            _config.Set("online_steam_appid", ((int)_numSteamAppId.Value).ToString());
            _config.Set("online_steam_max_players", ((int)_numRoomMax.Value).ToString());
            _config.Set("online_steam_public", _chkSteamPublic.Checked ? "1" : "0");
            _config.Set("online_steam_autohost", _chkSteamAutoHost.Checked ? "1" : "0");
            _config.Save();
        }

        // ---------------------------------------------------------------
        // Wire helpers (little-endian, as sh_net_proto.h)
        // ---------------------------------------------------------------

        private static void PutU8(byte[] b, ref int o, int v) { b[o++] = (byte)v; }

        private static void PutU16(byte[] b, ref int o, int v)
        {
            b[o++] = (byte)(v & 0xFF);
            b[o++] = (byte)((v >> 8) & 0xFF);
        }

        private static void PutU32(byte[] b, ref int o, uint v)
        {
            b[o++] = (byte)(v & 0xFF);
            b[o++] = (byte)((v >> 8) & 0xFF);
            b[o++] = (byte)((v >> 16) & 0xFF);
            b[o++] = (byte)((v >> 24) & 0xFF);
        }

        private static uint GetU32(byte[] b, int o)
        {
            return (uint)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24));
        }

        private static void PutStr(byte[] b, ref int o, string s, int cap)
        {
            for (int i = 0; i < cap; i++)
                b[o + i] = (byte)(i < s.Length && s[i] >= 32 && s[i] <= 126 ? s[i] : 0);
            b[o + cap - 1] = 0;
            o += cap;
        }

        private static string GetStr(byte[] b, ref int o, int cap)
        {
            int n = 0;
            while (n < cap && o + n < b.Length && b[o + n] != 0) n++;
            string s = Encoding.ASCII.GetString(b, o, n);
            o += cap;
            return s;
        }

        /// <summary>SipHash-2-4, as ShnSipHash in sh_net_proto.h.</summary>
        private static ulong SipHash(byte[] key, byte[] data)
        {
            ulong k0 = 0, k1 = 0;
            for (int i = 0; i < 8; i++)
            {
                k0 |= (ulong)key[i] << (8 * i);
                k1 |= (ulong)key[8 + i] << (8 * i);
            }
            ulong v0 = 0x736f6d6570736575UL ^ k0;
            ulong v1 = 0x646f72616e646f6dUL ^ k1;
            ulong v2 = 0x6c7967656e657261UL ^ k0;
            ulong v3 = 0x7465646279746573UL ^ k1;
            int len = data.Length;
            ulong b = (ulong)len << 56;
            int p = 0;
            for (; len >= 8; len -= 8, p += 8)
            {
                ulong m = 0;
                for (int i = 0; i < 8; i++) m |= (ulong)data[p + i] << (8 * i);
                v3 ^= m;
                SipRound(ref v0, ref v1, ref v2, ref v3);
                SipRound(ref v0, ref v1, ref v2, ref v3);
                v0 ^= m;
            }
            for (int i = 0; i < len; i++) b |= (ulong)data[p + i] << (8 * i);
            v3 ^= b;
            SipRound(ref v0, ref v1, ref v2, ref v3);
            SipRound(ref v0, ref v1, ref v2, ref v3);
            v0 ^= b;
            v2 ^= 0xff;
            for (int i = 0; i < 4; i++) SipRound(ref v0, ref v1, ref v2, ref v3);
            return v0 ^ v1 ^ v2 ^ v3;
        }

        private static ulong Rotl(ulong x, int b) { return (x << b) | (x >> (64 - b)); }

        private static void SipRound(ref ulong v0, ref ulong v1, ref ulong v2, ref ulong v3)
        {
            v0 += v1; v1 = Rotl(v1, 13); v1 ^= v0; v0 = Rotl(v0, 32);
            v2 += v3; v3 = Rotl(v3, 16); v3 ^= v2;
            v0 += v3; v3 = Rotl(v3, 21); v3 ^= v0;
            v2 += v1; v1 = Rotl(v1, 17); v1 ^= v2; v2 = Rotl(v2, 32);
        }

        /// <summary>ShnPasswordProof: the password bound to the server's cookie.</summary>
        private static uint PasswordProof(string pw, uint cookie)
        {
            if (string.IsNullOrEmpty(pw)) return 0;
            byte[] a = Encoding.ASCII.GetBytes("SHO1-pw-derive-A");
            byte[] bk = Encoding.ASCII.GetBytes("SHO1-pw-derive-B");
            byte[] pwb = Encoding.ASCII.GetBytes(pw);
            ulong h0 = SipHash(a, pwb);
            ulong h1 = SipHash(bk, pwb);
            var key = new byte[16];
            for (int i = 0; i < 8; i++)
            {
                key[i] = (byte)(h0 >> (8 * i));
                key[8 + i] = (byte)(h1 >> (8 * i));
            }
            var c = new byte[8];
            for (int i = 0; i < 4; i++) c[i] = (byte)(cookie >> (8 * i));
            c[4] = (byte)'p'; c[5] = (byte)'r'; c[6] = (byte)'o'; c[7] = (byte)'f';
            uint proof = (uint)(SipHash(key, c) & 0xFFFFFFFFUL);
            return proof == 0 ? 1u : proof;
        }

        /// <summary>
        /// The build id the game sends: the first 23 hex characters of the
        /// SHA-256 of SilentHillPC.exe, so a server with a build allow-list
        /// treats the test like the game itself.
        /// </summary>
        private static string BuildId()
        {
            try
            {
                string exe = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SilentHillPC.exe");
                if (!File.Exists(exe)) return "launcher";
                using (var sha = SHA256.Create())
                using (var f = File.OpenRead(exe))
                {
                    byte[] h = sha.ComputeHash(f);
                    var sb = new StringBuilder();
                    foreach (byte x in h) sb.Append(x.ToString("x2"));
                    return sb.ToString().Substring(0, BuildMax - 1);
                }
            }
            catch
            {
                return "launcher";
            }
        }

        private static int BuildHello(byte[] buf, string name, string build, uint proof, uint cookie)
        {
            int o = HdrSize;
            PutU16(buf, ref o, ProtoVer);
            PutU8(buf, ref o, 0);
            PutU8(buf, ref o, 0);
            PutU32(buf, ref o, 0);
            PutStr(buf, ref o, string.IsNullOrEmpty(name) ? "Wanderer" : name, NameMax);
            PutStr(buf, ref o, build, BuildMax);
            PutU32(buf, ref o, proof);
            PutU32(buf, ref o, cookie);
            PutU32(buf, ref o, 0); /* uid: the launcher is nobody */
            int h = 0;
            PutU8(buf, ref h, 'S'); PutU8(buf, ref h, 'H');
            PutU8(buf, ref h, 'O'); PutU8(buf, ref h, '1');
            PutU8(buf, ref h, ProtoVer);
            PutU8(buf, ref h, MsgHello);
            PutU16(buf, ref h, o - HdrSize);
            PutU32(buf, ref h, 0);
            return o;
        }

        private void BtnTest_Click(object sender, EventArgs e)
        {
            _lblStatus.ForeColor = SystemColors.ControlText;
            _lblStatus.Text = "Contacting...";
            _btnTest.Enabled = false;
            Application.DoEvents();
            try
            {
                string message;
                bool ok = TryHandshake(_txtServer.Text.Trim(), (int)_numPort.Value,
                                       _txtName.Text, _txtPassword.Text, out message);
                _lblStatus.ForeColor = ok ? Color.FromArgb(0, 110, 0) : Color.FromArgb(150, 0, 0);
                _lblStatus.Text = ok ? "Connected." : "Failed.";
                MessageBox.Show(this, message, "Silent Hill Online", MessageBoxButtons.OK,
                                ok ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
            }
            finally
            {
                _btnTest.Enabled = true;
            }
        }

        private bool TryHandshake(string host, int port, string name, string password, out string message)
        {
            message = "";
            if (string.IsNullOrEmpty(host))
            {
                message = "Enter a server address first.";
                return false;
            }

            try
            {
                using (var sock = new UdpClient())
                {
                    sock.Client.ReceiveTimeout = 2500;
                    string build = BuildId();
                    var buf = new byte[HdrSize + 128];
                    uint cookie = 0;
                    int n = BuildHello(buf, name, build, 0, 0);
                    sock.Send(buf, n, host, port);

                    // HELLO, CHALLENGE, HELLO with the cookie, WELCOME: at most a
                    // few round trips, each retried once for a NAT that has not
                    // opened yet.
                    for (int attempt = 0; attempt < 6; attempt++)
                    {
                        byte[] reply;
                        try
                        {
                            IPEndPoint from = null;
                            reply = sock.Receive(ref from);
                        }
                        catch (SocketException)
                        {
                            if (attempt >= 3) break;
                            n = BuildHello(buf, name, build, PasswordProof(password, cookie), cookie);
                            sock.Send(buf, n, host, port);
                            continue;
                        }
                        if (reply.Length < HdrSize) continue;
                        if (reply[0] != 'S' || reply[1] != 'H' || reply[2] != 'O' || reply[3] != '1')
                            continue;
                        if (reply[4] != ProtoVer)
                        {
                            message = "That server speaks protocol v" + reply[4] +
                                      "; this build speaks v" + ProtoVer + ".";
                            return false;
                        }

                        int type = reply[5];
                        int p = HdrSize;
                        if (type == MsgChallenge && reply.Length >= HdrSize + 4)
                        {
                            cookie = GetU32(reply, HdrSize);
                            n = BuildHello(buf, name, build, PasswordProof(password, cookie), cookie);
                            sock.Send(buf, n, host, port);
                            continue;
                        }
                        if (type == MsgWelcome)
                        {
                            p += 4 + 4 + 2 + 2 + 2 + 2;
                            string srv = GetStr(reply, ref p, ServerMax);
                            string motd = GetStr(reply, ref p, MotdMax);
                            message = "Connected to \"" + srv + "\" as " + (string.IsNullOrEmpty(name) ? "Wanderer" : name) +
                                      (motd.Length > 0 ? ".\r\n\r\n" + motd : ".");
                            // Leave again: the test is not a visit.
                            var bye = new byte[HdrSize];
                            int h = 0;
                            PutU8(bye, ref h, 'S'); PutU8(bye, ref h, 'H');
                            PutU8(bye, ref h, 'O'); PutU8(bye, ref h, '1');
                            PutU8(bye, ref h, ProtoVer);
                            PutU8(bye, ref h, MsgBye);
                            PutU16(bye, ref h, 0);
                            PutU32(bye, ref h, GetU32(reply, HdrSize));
                            sock.Send(bye, bye.Length, host, port);
                            return true;
                        }
                        if (type == MsgReject)
                        {
                            p += 4;
                            string why = GetStr(reply, ref p, RejectMax);
                            message = "The server refused: " + (why.Length > 0 ? why : "no reason given") +
                                      (why.IndexOf("build", StringComparison.OrdinalIgnoreCase) >= 0
                                           ? "\r\n\r\nThe server only admits builds on its list; this game's id is " + build + "."
                                           : "");
                            return false;
                        }
                    }
                    message = "No reply from " + host + ":" + port +
                              ".\r\n\r\nIs the server running, and is UDP " + port + " reachable from here?";
                    return false;
                }
            }
            catch (Exception ex)
            {
                message = ex.Message;
                return false;
            }
        }

        /// <summary>
        /// Runs online_server\steam_probe.exe and shows what it found. A
        /// separate process on purpose: the probe links the game's own Steam
        /// layer, so a mismatched steam_api64.dll takes down a throwaway process
        /// instead of the launcher.
        /// </summary>
        private void BtnSteamCheck_Click(object sender, EventArgs e)
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string exe = Path.Combine(baseDir, "online_server\\steam_probe.exe");
            if (!File.Exists(exe))
                exe = Path.Combine(baseDir, "steam_probe.exe");
            if (!File.Exists(exe))
            {
                MessageBox.Show(this,
                    "steam_probe.exe was not found next to the launcher or in online_server\\.",
                    "Silent Hill Online", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            _btnSteamCheck.Enabled = false;
            _lblSteamState.Text = "Checking...";
            Application.DoEvents();
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = exe,
                    Arguments = ((int)_numSteamAppId.Value).ToString(),
                    WorkingDirectory = Path.GetDirectoryName(exe),
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                string output;
                int code;
                using (var p = Process.Start(psi))
                {
                    output = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
                    p.WaitForExit(30000);
                    code = p.HasExited ? p.ExitCode : -1;
                }
                _lblSteamState.Text = code == 0
                    ? "Steam is working: lobbies, invites and peer-to-peer all responded."
                    : "Steam did not come up; see the details.";
                _lblSteamState.ForeColor = code == 0 ? Color.FromArgb(0, 110, 0)
                                                     : Color.FromArgb(150, 0, 0);
                ShowProbeOutput(output);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "Silent Hill Online",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                _btnSteamCheck.Enabled = true;
            }
        }

        private void ShowProbeOutput(string text)
        {
            using (var dlg = new Form())
            {
                dlg.Text = "Steam check";
                dlg.StartPosition = FormStartPosition.CenterParent;
                dlg.ClientSize = new Size(680, 460);
                var box = new TextBox
                {
                    Multiline = true,
                    ReadOnly = true,
                    ScrollBars = ScrollBars.Vertical,
                    Dock = DockStyle.Fill,
                    Font = new Font(FontFamily.GenericMonospace, 8.5f),
                    Text = text.Replace("\n", "\r\n").Replace("\r\r\n", "\r\n")
                };
                dlg.Controls.Add(box);
                dlg.ShowDialog(this);
            }
        }

        // ---------------------------------------------------------------
        // Hosting a living world server
        // ---------------------------------------------------------------

        private void UpdateHostState()
        {
            bool running = _server != null && !_server.HasExited;
            _btnHost.Enabled = !running;
            _btnHostStop.Enabled = running;
            _lblHostState.Text = running
                ? "Running on UDP " + (int)_numPort.Value + " (its console window shows who joins)."
                : "Not running.";
        }

        private void BtnHost_Click(object sender, EventArgs e)
        {
            if (_server != null && !_server.HasExited)
            {
                UpdateHostState();
                return;
            }

            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string exe = Path.Combine(baseDir, "online_server\\sh_master.exe");
            if (!File.Exists(exe))
                exe = Path.Combine(baseDir, "sh_master.exe");
            if (!File.Exists(exe))
            {
                MessageBox.Show(this,
                    "sh_master.exe was not found next to the launcher or in online_server\\.",
                    "Silent Hill Online", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            try
            {
                var args = "-p " + (int)_numPort.Value;
                if (_txtPassword.Text.Length > 0)
                    args += " -w \"" + _txtPassword.Text.Replace("\"", "") + "\"";
                _server = Process.Start(new ProcessStartInfo
                {
                    FileName = exe,
                    Arguments = args,
                    WorkingDirectory = Path.GetDirectoryName(exe),
                    UseShellExecute = true
                });
                UpdateHostState();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "Silent Hill Online",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void BtnHostStop_Click(object sender, EventArgs e)
        {
            try
            {
                if (_server != null && !_server.HasExited)
                {
                    _server.Kill();
                    _server.WaitForExit(3000);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "Silent Hill Online",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            UpdateHostState();
        }
    }
}
