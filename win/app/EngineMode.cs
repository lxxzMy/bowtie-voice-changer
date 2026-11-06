// EngineMode.cs — hot mode-switch P/Invoke (kept separate from Engine.cs to
// dodge a stubborn CS0117 that plagued it inside that file)
using System.Runtime.InteropServices;

namespace BowtieVC;

public static class EngineMode
{
    [DllImport("mvc_engine.dll", CharSet = CharSet.Ansi)]
    public static extern int mvcSetMode(int mode);

    [DllImport("mvc_engine.dll")]
    public static extern void mvcSetGains(double inGain, double outGain);
}
