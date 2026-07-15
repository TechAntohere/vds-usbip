using Microsoft.Win32;

namespace VdsTray;

/// <summary>Start-on-boot via the per-user Run key (no elevation needed).</summary>
internal static class Autostart
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "VdsTray";

    public static bool IsEnabled()
    {
        using var k = Registry.CurrentUser.OpenSubKey(RunKey);
        return k?.GetValue(ValueName) is not null;
    }

    public static void Set(bool enabled)
    {
        using var k = Registry.CurrentUser.OpenSubKey(RunKey, writable: true)
                      ?? Registry.CurrentUser.CreateSubKey(RunKey);
        if (k is null) return;
        if (enabled)
        {
            string exe = Environment.ProcessPath ?? "";
            if (exe.Length > 0) k.SetValue(ValueName, $"\"{exe}\"");
        }
        else
        {
            k.DeleteValue(ValueName, throwOnMissingValue: false);
        }
    }
}
