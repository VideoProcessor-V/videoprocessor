# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
# Get-VpPrimaryDisplayAdapter: the description of the adapter that drives the primary display
# (EnumDisplayDevices, DISPLAY_DEVICE_PRIMARY_DEVICE), or "" when it cannot be determined.
if (-not ("VpDisplayProbe" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class VpDisplayProbe
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DisplayDevice
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplayDevices(string device, uint index, ref DisplayDevice dd, uint flags);
    public static string Primary()
    {
        DisplayDevice d = new DisplayDevice();
        d.cb = Marshal.SizeOf(d);
        for (uint i = 0; EnumDisplayDevices(null, i, ref d, 0); i++)
        {
            if ((d.StateFlags & 4) != 0) return d.DeviceString;
            d.cb = Marshal.SizeOf(d);
        }
        return "";
    }
}
'@
}
function Get-VpPrimaryDisplayAdapter { [VpDisplayProbe]::Primary() }
