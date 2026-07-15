using System.Drawing;
using System.IO;
using System.Windows;
using System.Windows.Threading;
using WinForms = System.Windows.Forms;

namespace VdsTray;

internal static class Log
{
    private static readonly string Path =
        System.IO.Path.Combine(AppContext.BaseDirectory, "vdstray.log");
    public static void Write(string m)
    {
        try { File.AppendAllText(Path, $"{DateTime.Now:HH:mm:ss.fff}  {m}\n"); } catch { }
    }
}

public partial class App : System.Windows.Application
{
    private WinForms.NotifyIcon _tray = null!;
    private DispatcherTimer _poll = null!;
    private ConnectionPopup? _popup;
    private ControllerStatus? _prev;
    private static Mutex? _singleInstance;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // Single instance.
        _singleInstance = new Mutex(initiallyOwned: true, "VdsTray.SingleInstance", out bool isNew);
        if (!isNew) { Shutdown(); return; }

        // Bring the bridge up in the background (single-manager app).
        Task.Run(() =>
        {
            Vds.EnsureVdsd();
            for (int i = 0; i < 20 && !Vds.VdsdRunning(); i++) Thread.Sleep(250);
            Vds.EnsureAttached();
        });

        SetupTray();

        _poll = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _poll.Tick += (_, _) => Poll();
        _poll.Start();
        Poll();
    }

    private void SetupTray()
    {
        _tray = new WinForms.NotifyIcon
        {
            Icon = SystemIcons.Application, // TODO: replace with a DualSense .ico
            Visible = true,
            Text = "vDS — starting…",
        };

        var menu = new WinForms.ContextMenuStrip();

        var showItem = new WinForms.ToolStripMenuItem("Show status", null, (_, _) => ShowPopup(force: true));
        menu.Items.Add(showItem);

        var bootItem = new WinForms.ToolStripMenuItem("Start on boot") { Checked = Autostart.IsEnabled() };
        bootItem.Click += (_, _) =>
        {
            bool next = !bootItem.Checked;
            Autostart.Set(next);
            bootItem.Checked = next;
        };
        menu.Items.Add(bootItem);

        var reconnect = new WinForms.ToolStripMenuItem("Reconnect", null, (_, _) =>
            Task.Run(() => { Vds.EnsureVdsd(); Vds.EnsureAttached(); }));
        menu.Items.Add(reconnect);

        menu.Items.Add(new WinForms.ToolStripSeparator());
        menu.Items.Add(new WinForms.ToolStripMenuItem("Exit", null, (_, _) => ExitApp()));

        _tray.ContextMenuStrip = menu;
        // Left-click (single) shows the status popup; right-click opens the menu.
        _tray.MouseClick += (_, e) =>
        {
            if (e.Button == WinForms.MouseButtons.Left) ShowPopup(force: true);
        };
    }

    private void Poll()
    {
        ControllerStatus? cur = Vds.Status();
        Log.Write($"poll: connected={cur?.Connected} battery={cur?.Battery} hz={cur?.PollingHz} prevConn={_prev?.Connected}");

        // Tooltip.
        _tray.Text = cur is { Connected: true }
            ? $"DualSense — {cur.Battery}% · {cur.PollingHz} Hz"
            : "vDS — no controller";

        bool wasConnected = _prev is { Connected: true };
        bool nowConnected = cur is { Connected: true };

        // On connect: player LED once + lightbar off (once wired), and show popup.
        if (nowConnected && !wasConnected)
        {
            ShowPopup(cur!, force: true);
        }
        else if (nowConnected && _popup is { IsVisible: true })
        {
            _popup.Update(cur!); // live-update an already-visible popup
        }

        // Small transient popups on jack / mic changes.
        if (wasConnected && nowConnected && _prev is not null && cur is not null)
        {
            if (_prev.HeadphoneJack != cur.HeadphoneJack)
                ShowMini("headphones", cur.HeadphoneJack);
            if (_prev.MicMuted != cur.MicMuted)
                ShowMini("mic", !cur.MicMuted);
        }

        _prev = cur;
    }

    private void ShowPopup(bool force) { if (_prev is { Connected: true }) ShowPopup(_prev, force); }

    private void ShowPopup(ControllerStatus s, bool force)
    {
        try
        {
            _popup ??= new ConnectionPopup();
            _popup.Update(s);
            _popup.ShowFading(); // fades out after 5s
            Log.Write($"ShowPopup ok: L={_popup.Left} T={_popup.Top} W={_popup.ActualWidth} H={_popup.ActualHeight} vis={_popup.IsVisible} op={_popup.Opacity}");
        }
        catch (Exception ex) { Log.Write("ShowPopup EX: " + ex); }
    }

    private void ShowMini(string kind, bool on)
    {
        (_popup ??= new ConnectionPopup()).ShowMini(kind, on);
    }

    private void ExitApp()
    {
        try { _tray.Visible = false; _tray.Dispose(); } catch { }
        Shutdown();
    }
}
