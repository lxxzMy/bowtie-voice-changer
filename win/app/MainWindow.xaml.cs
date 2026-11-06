using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace BowtieVC;

public sealed partial class MainWindow : Window
{
    private static readonly string ExeDir = AppContext.BaseDirectory;
    private static readonly string CardsDir = Path.Combine(ExeDir, "cards");
    private string _modelsDir = Path.Combine(ExeDir, "models");
    private string _cableInstaller = Path.Combine(ExeDir, "redist", "VBCABLE_Setup_x64.exe");

    private bool _running;
    private bool _loading;
    private bool _engineReady;
    private int _switching;                   // 1 while a hot mode swap runs
    private readonly DispatcherTimer _statusTimer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private readonly DispatcherTimer _saveTimer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private readonly Microsoft.UI.Dispatching.DispatcherQueue _dq;
    private string _devSpk = "";

    // ---- settings (config.json next to the exe) ----
    public sealed class Config
    {
        public double preRollMs { get; set; } = 40;
        public double fadeMs { get; set; } = 2;
        public double inGain { get; set; } = 1.0;
        public double outGain { get; set; } = 1.0;
        public int ortThreads { get; set; } = 2;
        public int mode { get; set; } = 0;          // 0 = 40ms fast, 1 = 160ms quality
        public string? inDevId { get; set; }
        public string? outDevId { get; set; }
        public string? card { get; set; }
    }
    private readonly Config _cfg = new();
    private string ConfigPath => Path.Combine(ExeDir, "config.json");

    private void LoadConfig()
    {
        try
        {
            if (!File.Exists(ConfigPath)) return;
            var loaded = System.Text.Json.JsonSerializer.Deserialize<Config>(
                File.ReadAllText(ConfigPath));
            if (loaded != null)
            {
                _cfg.preRollMs = loaded.preRollMs; _cfg.fadeMs = loaded.fadeMs;
                _cfg.inGain = loaded.inGain; _cfg.outGain = loaded.outGain;
                _cfg.ortThreads = loaded.ortThreads;
                _cfg.inDevId = loaded.inDevId; _cfg.outDevId = loaded.outDevId;
                _cfg.card = loaded.card;
            }
        }
        catch { /* corrupt config → defaults */ }
    }

    private void SaveConfigNow()
    {
        try
        {
            var tmp = ConfigPath + ".tmp";
            File.WriteAllText(tmp, System.Text.Json.JsonSerializer.Serialize(_cfg,
                new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
            File.Move(tmp, ConfigPath, true);
        }
        catch { /* best effort */ }
    }

    private void ScheduleSave()
    {
        if (_loading) return;
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    private Microsoft.UI.Xaml.Media.Animation.Storyboard? _pulse;

    public MainWindow()
    {
        InitializeComponent();
        Title = "蝴蝶结变声器";
        try { this.SystemBackdrop = new Microsoft.UI.Xaml.Media.MicaBackdrop()
               { Kind = Microsoft.UI.Composition.SystemBackdrops.MicaKind.BaseAlt }; }
        catch { /* pre-Win11: fall back to default backdrop */ }
        _dq = this.DispatcherQueue;
        _statusTimer.Tick += (_, _) => RefreshStatus();
        _saveTimer.Tick += (_, _) => { _saveTimer.Stop(); SaveConfigNow(); };

        // apply persisted settings (flag suppresses save-on-load churn)
        LoadConfig();
        _loading = true;
        ModeCombo.SelectedIndex = _cfg.mode == 1 ? 1 : 0;
        if (PreRollSlider != null) PreRollSlider.Value = _cfg.preRollMs;
        if (FadeSlider != null) FadeSlider.Value = _cfg.fadeMs;
        if (InGainSlider != null) InGainSlider.Value = _cfg.inGain * 100.0;
        if (OutGainSlider != null) OutGainSlider.Value = _cfg.outGain * 100.0;
        if (ThreadsSlider != null) ThreadsSlider.Value = _cfg.ortThreads;
        _loading = false;

        this.Closed += (_, _) => { _saveTimer.Stop(); SaveConfigNow(); };

        // dev fallback: run from the repo uses MeanVC2's models/card directly
        // (exe is at win/app/bin/Release/<tfm>/win-x64/ → 6 levels up = repo root)
        string devModels = Path.GetFullPath(Path.Combine(ExeDir, "..", "..", "..", "..", "..", "..", "MeanVC2", "onnx_models"));
        if (Directory.Exists(devModels)) _modelsDir = devModels;
        _devSpk = Path.Combine(Path.GetDirectoryName(devModels)!, "test_audio", "reference_1.spk.npy");
        _ = Task.Run(InitEngineAsync);
    }

    // ------------------------------------------------------------------ init
    private async Task InitEngineAsync()
    {
        // first-run: seed the cards dir with the shipped/dev default card
        Directory.CreateDirectory(CardsDir);
        var defaultCard = Path.Combine(CardsDir, "reference_1.spk.npy");
        if (!File.Exists(defaultCard))
        {
            var shipped = Path.Combine(ExeDir, "cards_src", "reference_1.spk.npy");
            if (File.Exists(shipped)) File.Copy(shipped, defaultCard);
            else if (File.Exists(_devSpk)) File.Copy(_devSpk, defaultCard);
        }

        var cards = ListCards();
        string spk = cards.FirstOrDefault() ?? defaultCard;
        int rc = EngineApi.mvcInit(_modelsDir, File.Exists(spk) ? spk : "", _cfg.ortThreads, _cfg.mode);
        if (rc != 0)
        {
            await _dq.InvokeAsync(() => SetStatus($"引擎初始化失败：{EngineApi.LastError()}", true));
            return;
        }

        await _dq.InvokeAsync(() =>
        {
            RefreshDevices();
            RefreshCards();
            RefreshVirtualCardRow();
            ToggleBtn.IsEnabled = cards.Count > 0;
            _engineReady = true;
            StatusDot.Fill = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.LimeGreen);
            SetStatus("就绪。选择音色和设备后点「开始变声」。");
            _statusTimer.Start();
        });
    }

    private List<string> ListCards() =>
        Directory.Exists(CardsDir)
            ? Directory.GetFiles(CardsDir, "*.spk.npy").OrderBy(Path.GetFileNameWithoutExtension).ToList()
            : new();

    // -------------------------------------------------------------- devices
    private void RefreshDevices()
    {
        try
        {
            var (cap, ren) = EngineApi.EnumDevices();
            FillDeviceCombo(InCombo, cap, preferCable: false, preferredId: _cfg.inDevId);
            FillDeviceCombo(OutCombo, ren, preferCable: true, preferredId: _cfg.outDevId);
        }
        catch (Exception ex) { SetStatus($"枚举设备失败：{ex.Message}", true); }
    }

    private static void FillDeviceCombo(ComboBox combo, List<EngineApi.DeviceInfo> devices,
                                        bool preferCable, string? preferredId)
    {
        combo.Items.Clear();
        foreach (var d in devices) combo.Items.Add(new ComboBoxItem { Content = d.Name, Tag = d.Id });
        // priority: remembered device > default > virtual-audio endpoint
        int pick = -1;
        if (!string.IsNullOrEmpty(preferredId))
            pick = devices.FindIndex(d => d.Id == preferredId);
        if (pick < 0) pick = devices.FindIndex(d => d.Default);
        if (pick < 0 && preferCable)
        {
            pick = devices.FindIndex(d =>
                d.Name.Contains("CABLE Input", StringComparison.OrdinalIgnoreCase) ||
                d.Name.Contains("Voicemeeter Input", StringComparison.OrdinalIgnoreCase));
        }
        if (pick < 0 && devices.Count > 0) pick = 0;
        combo.SelectedIndex = pick;
    }

    private async void ModeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        _cfg.mode = ModeCombo.SelectedIndex == 1 ? 1 : 0;
        ScheduleSave();
        if (_running || _engineReady == false || Interlocked.CompareExchange(ref _switching, 1, 0) == 1)
        {
            // running: locked by SetControlsEnabled anyway; before-ready: the
            // startup init will pick the saved mode up
            return;
        }
        SetStatus("正在切换引擎档位（重新加载模型，约 1 秒）…");
        int rc = await Task.Run(() => EngineMode.mvcSetMode(_cfg.mode));
        Interlocked.Exchange(ref _switching, 0);
        if (rc != 0)
            SetStatus($"切换档位失败：{EngineApi.LastError()}（已回滚）", true);
        else
            SetStatus($"引擎档位已切换为 {(_cfg.mode == 1 ? "160ms 稳" : "40ms 快")}，下次开始变声即用新档位。");
    }

    private void RefreshVirtualCardRow()
    {
        int vs = EngineApi.mvcIsCableInstalled();
        bool have = vs != 0;
        CableRow.Visibility = have ? Visibility.Collapsed : Visibility.Visible;
        if (have)
        {
            string what = (vs & 1) != 0 ? "VB-Cable" : "Voicemeeter";
            SetStatus($"就绪。已检测到虚拟声卡：{what}。输出设备应选它的虚拟输入，Discord 麦克风选对应的虚拟输出。");
        }
    }

    private void DeviceCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        if (sender == InCombo) _cfg.inDevId = (InCombo.SelectedItem as ComboBoxItem)?.Tag as string;
        else _cfg.outDevId = (OutCombo.SelectedItem as ComboBoxItem)?.Tag as string;
        ScheduleSave();
    }

    private void CardCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_running) return;
        if (CardCombo.SelectedItem is ComboBoxItem it && it.Tag is string path)
        {
            if (EngineApi.mvcSetCard(path) != 0) SetStatus($"换音色失败：{EngineApi.LastError()}", true);
            AuditionBtn.IsEnabled = File.Exists(RefWavFor(path));
            _cfg.card = path;
            ScheduleSave();
        }
    }

    private static string RefWavFor(string cardPath) =>
        Path.Combine(Path.GetDirectoryName(cardPath)!,
            Path.GetFileNameWithoutExtension(Path.GetFileNameWithoutExtension(cardPath)) + ".ref.wav");

    // ------------------------------------------------------------ start/stop
    private async void ToggleBtn_Click(object sender, RoutedEventArgs e)
    {
        if (!_running)
        {
            string? inId = (InCombo.SelectedItem as ComboBoxItem)?.Tag as string;
            string? outId = (OutCombo.SelectedItem as ComboBoxItem)?.Tag as string;
            double preRoll = PreRollSlider.Value;
            double fade = FadeSlider.Value;
            float inGain = (float)(InGainSlider.Value / 100.0);
            float outGain = (float)(OutGainSlider.Value / 100.0);
            _cfg.inDevId = inId; _cfg.outDevId = outId;
            ScheduleSave();
            SetStatus("正在启动（预热引擎）…");
            // mvcStart does warmup (hundreds of ms) — keep it off the UI thread
            int rc = await Task.Run(() => EngineApi.mvcStart(preRoll, fade, inGain, outGain, inId, outId));
            if (rc != 0) { SetStatus($"启动失败：{EngineApi.LastError()}", true); return; }
            _running = true;
            ToggleBtn.Content = "停止变声";
            SetControlsEnabled(false);
            StatusDot.Fill = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.LimeGreen);
            (_pulse ??= (Microsoft.UI.Xaml.Media.Animation.Storyboard)RootGrid.Resources["PulseStory"]).Begin();
        }
        else
        {
            await Task.Run(EngineApi.mvcStop);
            _running = false;
            ToggleBtn.Content = "开始变声";
            SetControlsEnabled(true);
            _pulse?.Stop();
            GlowRing.Opacity = 0;
            StatusDot.Opacity = 1;
            StatusDot.Fill = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Gray);
            SetStatus("已停止。");
        }
    }

    private void SetControlsEnabled(bool enabled)
    {
        CardCombo.IsEnabled = enabled;
        InCombo.IsEnabled = enabled;
        OutCombo.IsEnabled = enabled;
        CloneBtn.IsEnabled = enabled;
        ModeCombo.IsEnabled = enabled;
        PreRollSlider.IsEnabled = enabled;
        ThreadsSlider.IsEnabled = enabled;
        if (enabled) CardCombo_SelectionChanged(null, null!);
    }

    private void RefreshStatus()
    {
        EngineApi.mvcGetStatus(out var st);
        if (st.streamErr != 0 && _running)
        {
            _ = Task.Run(EngineApi.mvcStop);
            _running = false;
            ToggleBtn.Content = "开始变声";
            SetControlsEnabled(true);
            _pulse?.Stop();
            GlowRing.Opacity = 0;
            StatusDot.Opacity = 1;
            StatusDot.Fill = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Gray);
            SetStatus($"引擎出错已自动停止：{EngineApi.LastError()}", true);
            return;
        }
        // audio backend readout (WASAPI shared mode + negotiated periods)
        var ab = new byte[256];
        EngineApi.mvcGetAudioInfo(ab, ab.Length);
        int z = Array.IndexOf(ab, (byte)0);
        AudioInfoText.Text = System.Text.Encoding.UTF8.GetString(ab, 0, z < 0 ? ab.Length : z);
        StatusText.Text = st.running == 1
            ? $"运行中 · 引擎 {st.procMs:F0}ms/{(_cfg.mode == 1 ? 160 : 40)}ms 块 · 缓冲 {st.bufMs:F0}ms · 丢块 {st.drops} · 输出断流 {st.gaps}"
            : _running ? "停止中…" : StatusText.Text;
    }

    private void SetStatus(string msg, bool error = false)
    {
        // callable from any thread: marshal onto the UI thread
        if (!_dq.HasThreadAccess)
        {
            _dq.TryEnqueue(() => SetStatus(msg, error));
            return;
        }
        StatusText.Text = msg;
        StatusText.Foreground = error
            ? new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.OrangeRed)
            : new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Gray);
    }

    // ---------------------------------------------------------------- params
    private void Param_ValueChanged(object sender, RoutedEventArgs e)
    {
        if (PreRollVal != null && PreRollSlider != null)
            PreRollVal.Text = ((int)PreRollSlider.Value).ToString();
        if (FadeVal != null && FadeSlider != null)
            FadeVal.Text = ((int)FadeSlider.Value).ToString();
        if (InGainVal != null && InGainSlider != null)
            InGainVal.Text = ((int)InGainSlider.Value) + "%";
        if (OutGainVal != null && OutGainSlider != null)
            OutGainVal.Text = ((int)OutGainSlider.Value) + "%";
        if (ThreadsVal != null && ThreadsSlider != null)
            ThreadsVal.Text = ((int)ThreadsSlider.Value).ToString();
        if (_loading) return;
        // gains apply LIVE while running; the rest take effect at next start
        if (InGainSlider != null && OutGainSlider != null)
        {
            double ig = InGainSlider.Value / 100.0, og = OutGainSlider.Value / 100.0;
            _ = Task.Run(() => EngineMode.mvcSetGains(ig, og));
        }
        if (PreRollSlider != null) _cfg.preRollMs = PreRollSlider.Value;
        if (FadeSlider != null) _cfg.fadeMs = FadeSlider.Value;
        if (InGainSlider != null) _cfg.inGain = InGainSlider.Value / 100.0;
        if (OutGainSlider != null) _cfg.outGain = OutGainSlider.Value / 100.0;
        if (ThreadsSlider != null) _cfg.ortThreads = (int)ThreadsSlider.Value;
        ScheduleSave();
    }

    // --------------------------------------------------------------- cloning
    private async void CloneBtn_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
        picker.FileTypeFilter.Add(".wav");
        var file = await picker.PickSingleFileAsync();
        if (file == null) return;

        SetStatus("正在克隆（首次要加载克隆编码器，可能需要几十秒）…");
        string name = Path.GetFileNameWithoutExtension(file.Name);
        string outNpy = Path.Combine(CardsDir, name + ".spk.npy");
        var errBuf = new byte[1024];
        int rc = await Task.Run(() => EngineApi.mvcCloneCard(file.Path, outNpy, errBuf, errBuf.Length));
        if (rc != 0)
        {
            SetStatus($"克隆失败：{EngineApi.CloneError(errBuf)}", true);
            return;
        }
        RefreshCards();
        SetStatus($"克隆完成：{name}");
    }

    private void RefreshCards()
    {
        var sel = (CardCombo.SelectedItem as ComboBoxItem)?.Tag as string ?? _cfg.card;
        CardCombo.Items.Clear();
        foreach (var card in ListCards())
            CardCombo.Items.Add(new ComboBoxItem { Content = Path.GetFileNameWithoutExtension(Path.GetFileNameWithoutExtension(card)), Tag = card });
        int idx = sel is null ? -1 : ListCards().IndexOf(sel);
        CardCombo.SelectedIndex = idx >= 0 ? idx : (ListCards().Count > 0 ? 0 : -1);
    }

    private async void AuditionBtn_Click(object sender, RoutedEventArgs e)
    {
        if (CardCombo.SelectedItem is ComboBoxItem it && it.Tag is string path)
        {
            var refWav = RefWavFor(path);
            if (File.Exists(refWav))
            {
                try { Process.Start(new ProcessStartInfo(refWav) { UseShellExecute = true }); }
                catch (Exception ex) { SetStatus($"试听失败：{ex.Message}", true); }
            }
            await Task.CompletedTask;
        }
    }

    private async void InstallCableBtn_Click(object sender, RoutedEventArgs e)
    {
        if (!File.Exists(_cableInstaller))
        {
            SetStatus($"未找到 VB-Cable 安装包（{_cableInstaller}）。请从 https://vb-audio.com/Cable/ 下载安装。", true);
            return;
        }
        try
        {
            // silent install per VB-Audio licensing terms; needs elevation
            Process.Start(new ProcessStartInfo(_cableInstaller, "-i -h")
            {
                UseShellExecute = true,
                Verb = "runas"
            });
            SetStatus("正在安装虚拟声卡，装完后可能需要重启系统。装好后再打开本软件。");
        }
        catch (Exception ex)
        {
            SetStatus($"启动安装失败：{ex.Message}", true);
        }
        await Task.CompletedTask;
    }
}

// helper for marshalling onto the UI thread
internal static class DispatcherQueueUIThread
{
    public static Task InvokeAsync(this Microsoft.UI.Dispatching.DispatcherQueue q, Action a)
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var ok = q.TryEnqueue(() => { try { a(); tcs.SetResult(); } catch (Exception ex) { tcs.SetException(ex); } });
        if (!ok) tcs.SetException(new InvalidOperationException("DispatcherQueue enqueue failed"));
        return tcs.Task;
    }
}
