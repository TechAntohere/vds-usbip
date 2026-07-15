using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace VdsTray;

/// <summary>Paths to the vDS binaries. Adjust here (or later read from config).</summary>
internal static class Paths
{
    public static string RepoRoot = @"C:\Users\Antonio\Documents\vds";
    public static string Build => System.IO.Path.Combine(RepoRoot, "build");
    public static string Vdsd => System.IO.Path.Combine(Build, "vdsd.exe");
    public static string Vdsctl => System.IO.Path.Combine(Build, "vdsctl.exe");
    public static string Usbip = @"C:\Program Files\USBip\usbip.exe";
    public static string BusId = "1-1";
}

/// <summary>One controller's live status, parsed from `vdsctl status` JSON.</summary>
public sealed record ControllerStatus
{
    public string Address { get; init; } = "";
    public bool Connected { get; init; }
    public int Battery { get; init; } = -1;
    public int ChargeStatus { get; init; } = -1; // 0 discharging,1 charging,2 full
    public bool HeadphoneJack { get; init; }
    public bool MicDetected { get; init; }
    public bool MicMuted { get; init; }
    public bool MicActive { get; init; }
    public bool SpeakerActive { get; init; }
    public int PollingHz { get; init; }
    public int MicGain { get; init; } = -1;
    public bool HasInput { get; init; }

    public bool Charging => ChargeStatus == 1;
    public bool ChargeComplete => ChargeStatus == 2;
}

/// <summary>Thin wrapper over vdsctl / usbip for the tray app.</summary>
internal static class Vds
{
    /// <summary>Runs a process, returns stdout (empty on failure). Never throws.</summary>
    private static string Run(string exe, string args, int timeoutMs = 4000)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                Arguments = args,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            using var p = Process.Start(psi);
            if (p is null) return "";
            string outText = p.StandardOutput.ReadToEnd();
            if (!p.WaitForExit(timeoutMs))
            {
                try { p.Kill(true); } catch { }
                return "";
            }
            return outText;
        }
        catch { return ""; }
    }

    /// <summary>Queries `vdsctl status`; returns the first connected controller, or null.</summary>
    public static ControllerStatus? Status()
    {
        string outText = Run(Paths.Vdsctl, "status");
        if (string.IsNullOrWhiteSpace(outText)) return null;
        // One JSON object per line; take the first parseable connected one.
        foreach (var line in outText.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            var s = Parse(line.Trim());
            if (s is not null && s.Connected) return s;
        }
        return null;
    }

    private static ControllerStatus? Parse(string json)
    {
        if (string.IsNullOrWhiteSpace(json) || json[0] != '{') return null;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            int GetInt(string k, int def = 0) =>
                r.TryGetProperty(k, out var v) && v.TryGetInt32(out var n) ? n : def;
            bool GetBool(string k) =>
                r.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.True;
            string GetStr(string k) =>
                r.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString()! : "";
            if (!r.TryGetProperty("connected", out _)) return null;
            return new ControllerStatus
            {
                Address = GetStr("address"),
                Connected = GetBool("connected"),
                Battery = GetInt("battery", -1),
                ChargeStatus = GetInt("charge_status", -1),
                HeadphoneJack = GetBool("headphone_jack"),
                MicDetected = GetBool("mic_detected"),
                MicMuted = GetBool("mic_muted"),
                MicActive = GetBool("mic_active"),
                SpeakerActive = GetBool("speaker_active"),
                PollingHz = GetInt("polling_hz"),
                MicGain = GetInt("mic_gain", -1),
                HasInput = GetBool("has_input"),
            };
        }
        catch { return null; }
    }

    // --- Bridge management (single-manager app) ------------------------------

    public static bool VdsdRunning() =>
        Process.GetProcessesByName("vdsd").Length > 0;

    /// <summary>Start vdsd (USB/IP transport) hidden if not already running.</summary>
    public static void EnsureVdsd()
    {
        if (VdsdRunning()) return;
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = Paths.Vdsd,
                WorkingDirectory = Paths.Build,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            psi.Environment["VDS_TRANSPORT"] = "usbip";
            Process.Start(psi);
        }
        catch { }
    }

    /// <summary>Attach the virtual device if not already imported.</summary>
    public static void EnsureAttached()
    {
        string port = Run(Paths.Usbip, "port");
        if (port.Contains("127.0.0.1:3240")) return;
        Run(Paths.Usbip, $"attach -r 127.0.0.1 -b {Paths.BusId}", timeoutMs: 8000);
    }

    /// <summary>Set the runtime mic gain (linear multiplier) via vdsctl.</summary>
    public static void SetMicGain(int gain) => Run(Paths.Vdsctl, $"set mic-gain {gain}");
}
