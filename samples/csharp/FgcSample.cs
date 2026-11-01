// Minimal C# front end for FgcCore using plain P/Invoke, no wrapper library.
// Build (from a folder containing FgcCore.dll):
//   csc /platform:x64 /out:FgcSample.exe FgcSample.cs
// Starts the keyboard whose hardware ID ends with the argument for two seconds, then stops it. That keyboard
// is hidden from Windows meanwhile, so pick one you are not typing on.
//   FgcSample.exe Col05
using System;
using System.Runtime.InteropServices;
using System.Text;

static class Fgc
{
    const string Dll = "FgcCore.dll";

    // Every struct starts with its own size; strings are fixed UTF-8 buffers.
    [StructLayout(LayoutKind.Sequential)]
    public struct KeyboardInfo
    {
        public uint StructSize;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] HardwareId;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)] public byte[] DisplayName;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int fgc_create(out IntPtr context);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void fgc_destroy(IntPtr context);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int fgc_refresh_keyboards(IntPtr context);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int fgc_get_keyboard_count(IntPtr context, out uint count);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int fgc_get_keyboard(IntPtr context, uint index, ref KeyboardInfo info);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int fgc_start(IntPtr context, [MarshalAs(UnmanagedType.LPUTF8Str)] string hardwareId);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int fgc_stop(IntPtr context, [MarshalAs(UnmanagedType.LPUTF8Str)] string hardwareId);

    static string Text(byte[] buffer)
    {
        int length = Array.IndexOf(buffer, (byte)0);
        return Encoding.UTF8.GetString(buffer, 0, length < 0 ? buffer.Length : length);
    }

    public static int Main(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("usage: FgcSample <end of a keyboard's hardware ID>");
            return 2;
        }
        string suffix = args[0];
        IntPtr context;
        if (fgc_create(out context) != 0) return 1;

        fgc_refresh_keyboards(context);
        uint count;
        fgc_get_keyboard_count(context, out count);
        for (uint i = 0; i < count; i++)
        {
            var info = new KeyboardInfo { StructSize = (uint)Marshal.SizeOf<KeyboardInfo>(), HardwareId = new byte[256], DisplayName = new byte[128] };
            fgc_get_keyboard(context, i, ref info);
            string id = Text(info.HardwareId);
            Console.WriteLine("[" + i + "] " + Text(info.DisplayName));
            if (!id.EndsWith(suffix)) continue;

            Console.WriteLine("start: " + fgc_start(context, id));
            System.Threading.Thread.Sleep(2000);
            Console.WriteLine("stop: " + fgc_stop(context, id));
            break;
        }

        fgc_destroy(context);
        return 0;
    }
}
