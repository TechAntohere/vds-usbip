using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;
using Color = System.Windows.Media.Color;
using Rectangle = System.Windows.Shapes.Rectangle;
using Path = System.IO.Path;

namespace VdsTray;

public partial class ConnectionPopup : Window
{
    private readonly DispatcherTimer _hide = new() { Interval = TimeSpan.FromSeconds(5) };

    // On/off appearance for the status indicators (white circle + dark glyph
    // when active; dark circle + grey glyph when inactive).
    private static readonly SolidColorBrush CircleOn = new(Color.FromRgb(0xED, 0xED, 0xED));
    private static readonly SolidColorBrush CircleOff = new(Color.FromRgb(0x3A, 0x3A, 0x3A));
    private static readonly SolidColorBrush IconOn = new(Color.FromRgb(0x1A, 0x1A, 0x1A));
    private static readonly SolidColorBrush IconOff = new(Color.FromRgb(0x7A, 0x7A, 0x7A));

    public ConnectionPopup()
    {
        InitializeComponent();
        _hide.Tick += (_, _) => { _hide.Stop(); FadeOut(); };
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
        HpMask.ImageSource = Load("headphones.png");
        MicMask.ImageSource = Load("mic.png");
    }

    private static void SetIndicator(Ellipse circle, Rectangle icon, bool on)
    {
        circle.Fill = on ? CircleOn : CircleOff;
        icon.Fill = on ? IconOn : IconOff;
    }

    /// <summary>Refresh the card fields from live status.</summary>
    public void Update(ControllerStatus s)
    {
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
        ColorValue.Text = string.IsNullOrEmpty(s.Color) || s.Color == "unknown"
            ? "Unknown" : s.Color;

        // Model: title + controller art. Prefer an Edge-specific asset when it
        // exists, else fall back to the standard DualSense art.
        TitleText.Text = s.IsEdge ? "DualSense Edge" : "Dualsense";
        ControllerImage.Source =
            (s.IsEdge ? Load("dualsense_edge.png") : null) ?? Load("dualsense.png");

        // Headphones "on" = 3.5mm jack present. Mic "on" = not muted (the
        // mute button toggles this; "recording active" almost never fires).
        SetIndicator(HpCircle, HpIcon, s.HeadphoneJack);
        SetIndicator(MicCircle, MicIcon, !s.MicMuted);
    }

    /// <summary>Show the full connection card, then fade out after 5s.</summary>
    /// <summary>True when the full connection card (not the mini toast) is on screen.</summary>
    public bool IsCardVisible =>
        IsVisible && ConnectionView.Visibility == Visibility.Visible;

    public void ShowFading()
    {
        ConnectionView.Visibility = Visibility.Visible;
        MiniView.Visibility = Visibility.Collapsed;
        FadeIn();
        PositionBottomRight();
        _hide.Stop();
        _hide.Start();
    }

    /// <summary>Show a small square toast with just an icon (jack/mic changes).</summary>
    /// <param name="kind">"headphones" or "mic".</param>
    /// <param name="on">Whether the thing turned on (white) or off (dimmed).</param>
    public void ShowMini(string kind, bool on)
    {
        MiniMask.ImageSource = Load($"{kind}.png");
        MiniCircle.Fill = on ? CircleOn : CircleOff;
        MiniIcon.Fill = on ? IconOn : IconOff;
        ConnectionView.Visibility = Visibility.Collapsed;
        MiniView.Visibility = Visibility.Visible;
        FadeIn();
        PositionBottomRight();
        _hide.Stop();
        _hide.Start();
    }

    private void PositionBottomRight()
    {
        if (ActualWidth <= 0 || ActualHeight <= 0) return;
        var wa = SystemParameters.WorkArea;
        Left = wa.Right - ActualWidth;
        Top = wa.Bottom - ActualHeight;
    }

    private void FadeIn()
    {
        BeginAnimation(OpacityProperty, null);
        Opacity = 0;
        Show();
        BeginAnimation(OpacityProperty,
            new DoubleAnimation(0.0, 1.0, TimeSpan.FromMilliseconds(180)));
    }

    private void FadeOut()
    {
        var anim = new DoubleAnimation(0.0, TimeSpan.FromMilliseconds(420));
        anim.Completed += (_, _) => Hide();
        BeginAnimation(OpacityProperty, anim);
    }
}
