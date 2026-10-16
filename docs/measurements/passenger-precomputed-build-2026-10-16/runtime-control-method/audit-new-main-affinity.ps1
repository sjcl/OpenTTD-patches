$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class MainThreadAffinityAudit {
 [StructLayout(LayoutKind.Sequential)] public struct GroupAffinity { public UIntPtr Mask; public ushort Group; public ushort Reserved0; public ushort Reserved1; public ushort Reserved2; }
 [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetThreadGroupAffinity(IntPtr thread, out GroupAffinity affinity);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
$taskDeadline = (Get-Date).AddMinutes(25)
while ($true) {
 $taskGame = Get-Process openttd -ErrorAction SilentlyContinue | Where-Object {$_.Path -like '*c430*runtime-main-core-new*'} | Select-Object -First 1
 if ($null -ne $taskGame) { break }
 if ((Get-Date) -gt $taskDeadline) { throw 'New benchmark process did not start' }
 Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 3
$taskMain = $taskGame.Threads | Sort-Object StartTime | Select-Object -First 1
$taskHandle = [MainThreadAffinityAudit]::OpenThread(64,$false,$taskMain.Id)
$taskAffinity = New-Object MainThreadAffinityAudit+GroupAffinity
$taskOk = [MainThreadAffinityAudit]::GetThreadGroupAffinity($taskHandle,[ref]$taskAffinity)
[void][MainThreadAffinityAudit]::CloseHandle($taskHandle)
$taskResult = [ordered]@{pid=$taskGame.Id;thread_id=$taskMain.Id;success=$taskOk;mask=$taskAffinity.Mask.ToUInt64();group=$taskAffinity.Group;process_mask=$taskGame.ProcessorAffinity.ToInt64()}
$taskResult | ConvertTo-Json | Set-Content -Encoding utf8 build-msvc\runtime-main-core-new\affinity-audit.json
$taskResult | ConvertTo-Json -Compress
