// smoke — P/Invoke surface test for mvc_engine.dll (W1 gate)
// 1) enum devices + cable detection
// 2) init engine with real models
// 3) file-sim conversion (same path as live, minus WASAPI)
// 4) status counters sanity
using System.Runtime.InteropServices;
using System.Text;

var exeDir = AppContext.BaseDirectory;
var root = Path.GetFullPath(Path.Combine(exeDir, "..", "..", "..", "..", ".."));
var dll = Path.Combine(root, "win", "engine", "mvc_engine.dll");
Console.WriteLine($"dll: {dll} exists={File.Exists(dll)}");

// 1) devices
var nb = new byte[64 * 1024];
int n = Smoke.MvcEnumDevices(nb, nb.Length);
if (n < 0) { Console.WriteLine("ENUM FAILED: " + Smoke.LastError()); return 1; }
var json = Encoding.UTF8.GetString(nb, 0, Math.Min(n - 1, nb.Length));
using (var doc = System.Text.Json.JsonDocument.Parse(json))
{
    foreach (var dir in new[] { "capture", "render" })
    {
        Console.WriteLine($"[{dir}]");
        foreach (var d in doc.RootElement.GetProperty(dir).EnumerateArray())
            Console.WriteLine($"  {(d.GetProperty("default").GetBoolean() ? "*" : " ")} {d.GetProperty("name").GetString()}");
    }
}
Console.WriteLine($"VB-Cable installed: {Smoke.MvcIsCableInstalled() == 1}");

// 2) init (real models)
var models = Path.Combine(root, "MeanVC2", "onnx_models");
var spk = Path.Combine(root, "MeanVC2", "test_audio", "reference_1.spk.npy");
Console.WriteLine($"init: models={models}");
var t0 = DateTime.Now;
int mode = args.Length > 0 && args[0] == "120" ? 1 : 0;
int rc = Smoke.MvcInit(models, spk, 1, mode);
if (rc != 0) { Console.WriteLine("INIT FAILED: " + Smoke.LastError()); return 1; }
Console.WriteLine($"init ok in {(DateTime.Now - t0).TotalSeconds:F1}s");

// 3) sim
var wavIn = Environment.GetEnvironmentVariable("SIM_WAV") ?? Path.Combine(root, "MeanVC2", "test_audio", "source_en_1.wav");
var tag = Environment.GetEnvironmentVariable("SIM_TAG") ?? "";
var wavOut = Path.Combine(root, "win", "smoke", "sim_out" + tag + ".wav");
t0 = DateTime.Now;
rc = Smoke.MvcRunSim(wavIn, wavOut, 40);
if (rc != 0) { Console.WriteLine("SIM FAILED: " + Smoke.LastError()); return 1; }
Console.WriteLine($"sim ok in {(DateTime.Now - t0).TotalSeconds:F1}s -> {wavOut}");
var wav16 = Path.Combine(root, "win", "smoke", "sim16_out.wav");
rc = Smoke.MvcRunSim16(wavIn, wav16);
if (rc != 0) { Console.WriteLine("SIM16 FAILED: " + Smoke.LastError()); return 1; }
var condPath = Path.Combine(root, "win", "smoke", "cpp_conds.f32");
rc = Smoke.MvcExportCondDump(condPath);
if (rc != 0) { Console.WriteLine("DUMP FAILED: " + Smoke.LastError()); return 1; }
Console.WriteLine($"sim16 + cond dump ok -> {condPath}");

// hot mode switch: 40 -> 160, then re-run sim16 + dump
if (mode == 0)
{
    t0 = DateTime.Now;
    rc = Smoke.MvcSetMode(1);
    if (rc != 0) { Console.WriteLine("SETMODE FAILED: " + Smoke.LastError()); return 1; }
    Console.WriteLine($"hot switch to 160ms ok in {(DateTime.Now - t0).TotalSeconds:F1}s");
    rc = Smoke.MvcRunSim16(wavIn, Path.Combine(root, "win", "smoke", "sim16_out_120.wav"));
    if (rc != 0) { Console.WriteLine("SIM16-120 FAILED: " + Smoke.LastError()); return 1; }
    rc = Smoke.MvcExportCondDump(Path.Combine(root, "win", "smoke", "cpp_conds_120.f32"));
    if (rc != 0) { Console.WriteLine("DUMP-120 FAILED: " + Smoke.LastError()); return 1; }
    Console.WriteLine("post-switch sim16 + cond dump ok");
    rc = Smoke.MvcSetMode(0);
    if (rc != 0) { Console.WriteLine("SETMODE BACK FAILED: " + Smoke.LastError()); return 1; }
    Console.WriteLine("hot switch back to 40ms ok");
}

// 4) status
Smoke.MvcGetStatus(out var st);
Console.WriteLine($"status: proc={st.procMs:F1}ms feed={st.feedMs:F1}ms played48={st.played48 / 48000.0:F2}s gaps={st.gaps} preroll={st.preroll} drops={st.drops} err={st.streamErr}");
return st.gaps == 0 && st.streamErr == 0 && st.played48 > 0 ? 0 : 2;

internal static class Smoke
{
    [DllImport("mvc_engine.dll", EntryPoint = "mvcEnumDevices", CharSet = CharSet.Ansi)]
    internal static extern int MvcEnumDevices(byte[]? buf, int len);
    [DllImport("mvc_engine.dll", EntryPoint = "mvcIsCableInstalled")]
    internal static extern int MvcIsCableInstalled();
    [DllImport("mvc_engine.dll", EntryPoint = "mvcInit", CharSet = CharSet.Ansi)]
    internal static extern int MvcInit(string modelsDir, string spkPath, int ortThreads, int mode);
    [DllImport("mvc_engine.dll", EntryPoint = "mvcRunSim", CharSet = CharSet.Ansi)]
    internal static extern int MvcRunSim(string wavIn, string wavOut, double preRollMs);
    [DllImport("mvc_engine.dll", EntryPoint = "mvcRunSim16", CharSet = CharSet.Ansi)]
    internal static extern int MvcRunSim16(string wavIn, string wavOut);
    [DllImport("mvc_engine.dll", EntryPoint = "mvcExportCondDump", CharSet = CharSet.Ansi)]
    internal static extern int MvcExportCondDump(string path);
    [DllImport("mvc_engine.dll", EntryPoint = "mvcSetMode", CharSet = CharSet.Ansi)]
    internal static extern int MvcSetMode(int mode);
    [DllImport("mvc_engine.dll", EntryPoint = "mvcGetStatus")]
    internal static extern void MvcGetStatus(out Status s);

    [DllImport("mvc_engine.dll", EntryPoint = "mvcGetLastError", CharSet = CharSet.Ansi)]
    internal static extern int MvcGetLastError(byte[] buf, int len);

    [StructLayout(LayoutKind.Sequential)]
    internal struct Status
    {
        public double procMs, feedMs, bufMs;
        public long gaps, preroll, drops, odrops, in16, out16, played48;
        public int running, streamErr;
    }

    internal static string LastError()
    {
        var b = new byte[1024];
        MvcGetLastError(b, b.Length);
        int i = Array.IndexOf(b, (byte)0);
        return Encoding.UTF8.GetString(b, 0, i < 0 ? b.Length : i);
    }
}
