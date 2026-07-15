using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Color = System.Windows.Media.Color;

namespace VdsTray;

public partial class ConnectionPopup : Window
{
    private readonly DispatcherTimer _hide = new() { Interval = TimeSpan.FromSeconds(5) };

    // On/off brushes for the status circles (white bg = active).
    private static readonly SolidColorBrush OnBrush =
        new(Color.FromRgb(0xED, 0xED, 0xED));
    private static readonly SolidColorBrush OffBrush =
        new(Color.FromRgb(0x3A, 0x3A, 0x3A));

    public ConnectionPopup()
    {
        InitializeComponent();
        _hide.Tick += (_, _) => { _hide.Stop(); FadeOut(); };
        // SizeToContent means the real size is only known after a layout pass;
        // reposition whenever it changes so we always sit flush bottom-right.
        SizeChanged += (_, _) => PositionBottomRight();
        LoadArt();
    }

    private static BitmapImage? Load(string file)
    {
        try
        {
            string path = Path.Combine(AppContext.BaseDirectory, "assets", file);
            return File.Exists(path) ? new BitmapImage(new Uri(path)) : null;
        }
        catch { return null; }
    }

    private void LoadArt()
    {
        ControllerImage.Source = Load("dualsense.png");
        HpImage.Source = Load("headphones.png");
        MicImage.Source = Load("mic.png");
    }

    /// <summary>Refresh the card fields from live status (does not change visibility).</summary>
    public void Update(ControllerStatus s)
    {
        TitleText.Text = "Dualsense";
        StatusText.Text = s.Connected ? "Connected" : "Disconnected";
        StatusText.Foreground = new SolidColorBrush(
            s.Connected ? Color.FromRgb(0x2F, 0x6B, 0xFF) : Color.FromRgb(0x9A, 0x9A, 0x9A));

        if (s.Battery >= 0)
        {
            BatteryText.Text = s.Charging ? $"{s.Battery}⚡" : s.Battery.ToString();
            var green = Color.FromRgb(0x3F, 0xCF, 0x4A);
            var amber = Color.FromRgb(0xE0, 0xA5, 0x2A);
            var red = Color.FromRgb(0xE0, 0x4A, 0x3A);
            var col = s.Battery <= 15 ? red : s.Battery <= 35 ? amber : green;
            BatteryBox.BorderBrush = new SolidColorBrush(col);
            BatteryText.Foreground = new SolidColorBrush(col);
            BatteryBox.Visibility = Visibility.Visible;
        }
        else
        {
            BatteryBox.Visibility = Visibility.Collapsed;
        }

        PollingValue.Text = s.PollingHz > 0 ? $"{s.PollingHz} Hz" : "—";
        ColorValue.Text = "White"; // TODO: real color once command-protocol bridges

        // Status circles: white/bright when active, dimmed otherwise.
        // Headphones "on" = 3.5mm jack present. Mic "on" = active and not muted.
        bool hpOn = s.HeadphoneJack;
        bool micOn = s.MicActive && !s.MicMuted;
        HpCircle.Fill = hpOn ? OnBrush : OffBrush;
        HpBox.Opacity = hpOn ? 1.0 : 0.45;
        MicCircle.Fill = micOn ? OnBrush : OffBrush;
        MicBox.Opacity = micOn ? 1.0 : 0.45;
    }

    /// <summary>Show the full connection card, then fade out after 5s.</summary>
    public void ShowFading()
    {
        ConnectionView.Visibility = Visibility.Visible;
        MiniView.Visibility = Visibility.Collapsed;
        FadeIn();               // Show() triggers layout -> SizeChanged -> position
        PositionBottomRight();  // and once more in case size was already known
        _hide.Stop();
        _hide.Start();
    }

    /// <summary>Show a small toast (jack/mic changes), then fade out after 5s.</summary>
    public void ShowMini(string text)
    {
        MiniText.Text = text;
        ConnectionView.Visibility = Visibility.Collapsed;
        MiniView.Visibility = Visibility.Visible;
        FadeIn();
        PositionBottomRight();
        _hide.Stop();
        _hide.Start();
    }

    private void PositionBottomRight()
    {
        if (ActualWidth <= 0 || ActualHeight <= 0) return; // not laid out yet
        var wa = SystemParameters.WorkArea; // primary monitor, DIPs
        Left = wa.Right - ActualWidth;
        Top = wa.Bottom - ActualHeight;
    }

    private void FadeIn()
    {
        // Clear any prior opacity animation, then fade in from 0. Holding the
        // animation (default FillBehavior) keeps it at 1 until FadeOut.
        BeginAnimation(OpacityProperty, null);
        Opacity = 0;
        Show();
        var anim = new DoubleAnimation(0.0, 1.0, TimeSpan.FromMilliseconds(180));
        BeginAnimation(OpacityProperty, anim);
    }

    private void FadeOut()
    {
        var anim = new DoubleAnimation(0.0, TimeSpan.FromMilliseconds(420));
        anim.Completed += (_, _) => Hide();
        BeginAnimation(OpacityProperty, anim);
    }
}
