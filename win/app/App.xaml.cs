using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;

namespace BowtieVC;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        InitializeComponent();
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int MessageBoxW(IntPtr h, string text, string caption, uint type);

    protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
    {
        UnhandledException += (_, e) =>
        {
            try { File.WriteAllText(_errPath, e.Message + "\r\n\r\n" + e.Exception); } catch { }
        };
        try
        {
            _window = new MainWindow();
            _window.Activate();
        }
        catch (Exception ex)
        {
            try { File.WriteAllText(_errPath, ex.ToString()); } catch { }
            MessageBoxW(IntPtr.Zero, ex.ToString(), "BowtieVC 启动异常", 0);
            throw;
        }
    }

    private static readonly string _errPath =
        @"E:\Projects\tts\mean2vc\win\app\startup_error.txt";
}
