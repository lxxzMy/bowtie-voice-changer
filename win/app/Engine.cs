// EngineApi.cs — P/Invoke wrapper for mvc_engine.dll
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace BowtieVC;

[StructLayout(LayoutKind.Sequential)]
public struct MvcStatus
{
    public double procMs, feedMs, bufMs;
    public long gaps, preroll, drops, odrops, in16, out16, played48;
    public int running, streamErr;
}

public static class EngineApi
{
    private const string Dll = "mvc_engine.dll";

    [DllImport(Dll, CharSet = CharSet.Ansi)] public static extern int mvcInit(string modelsDir, string spkPath, int ortThreads, int mode);
    [DllImport(Dll)] public static extern void mvcShutdown();
    [DllImport(Dll, CharSet = CharSet.Ansi)] public static extern int mvcStart(double preRollMs, double fadeMs, float inGain, float outGain, string? inDevId, string? outDevId);
    [DllImport(Dll)] public static extern void mvcStop();
    [DllImport(Dll)] public static extern void mvcGetStatus(out MvcStatus s);
    [DllImport(Dll, CharSet = CharSet.Ansi)] public static extern int mvcSetCard(string spkPath);
    [DllImport(Dll, CharSet = CharSet.Ansi)] public static extern int mvcCloneCard(string wavPath, string outNpy, byte[] errBuf, int errLen);
    [DllImport(Dll, CharSet = CharSet.Ansi)] public static extern int mvcEnumDevices(byte[]? buf, int len);
    [DllImport(Dll)] public static extern int mvcIsCableInstalled();
    [DllImport(Dll, CharSet = CharSet.Ansi)] public static extern int mvcGetLastError(byte[] buf, int len);
    [DllImport(Dll, CharSet = CharSet.Ansi)] public static extern int mvcGetAudioInfo(byte[] buf, int len);

    public static string LastError()
    {
        var b = new byte[1024];
        mvcGetLastError(b, b.Length);
        int i = Array.IndexOf<byte>(b, 0);
        return Encoding.UTF8.GetString(b, 0, i < 0 ? b.Length : i);
    }

    public static string CloneError(byte[] errBuf)
    {
        int i = Array.IndexOf<byte>(errBuf, 0);
        return Encoding.UTF8.GetString(errBuf, 0, i < 0 ? errBuf.Length : i);
    }

    public sealed record DeviceInfo(string Id, string Name, bool Default);

    public static (List<DeviceInfo> capture, List<DeviceInfo> render) EnumDevices()
    {
        var b = new byte[256 * 1024];
        int n = mvcEnumDevices(b, b.Length);
        if (n < 0) throw new InvalidOperationException(LastError());
        var json = Encoding.UTF8.GetString(b, 0, Math.Min(n - 1, b.Length));
        using var doc = System.Text.Json.JsonDocument.Parse(json);
        static List<DeviceInfo> Parse(System.Text.Json.JsonElement e) =>
            e.EnumerateArray()
             .Select(d => new DeviceInfo(d.GetProperty("id").GetString() ?? "",
                                         d.GetProperty("name").GetString() ?? "",
                                         d.GetProperty("default").GetBoolean()))
             .ToList();
        return (Parse(doc.RootElement.GetProperty("capture")),
                Parse(doc.RootElement.GetProperty("render")));
    }
}
