param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,
  [Parameter(Mandatory = $true)]
  [string]$DataDirectory
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class WifiOptSmokeNative {
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder className, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr window, StringBuilder text, int maximumCount);

    [DllImport("user32.dll")]
    public static extern int GetDlgCtrlID(IntPtr window);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
}
'@

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedDataDirectory = (Resolve-Path -LiteralPath $DataDirectory).Path
$previousDataDirectory = $env:WIFIOPT_DATA_DIR
$process = $null
$window = [IntPtr]::Zero
$detailsWindow = [IntPtr]::Zero

try {
  $env:WIFIOPT_DATA_DIR = $resolvedDataDirectory
  $process = Start-Process -FilePath $resolvedExecutable -WindowStyle Hidden -PassThru
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  do {
    $script:window = [IntPtr]::Zero
    $script:detailsWindow = [IntPtr]::Zero
    $findWindow = [WifiOptSmokeNative+EnumWindowsProc]{
      param([IntPtr]$candidate, [IntPtr]$parameter)
      [uint32]$candidateProcessId = 0
      [void][WifiOptSmokeNative]::GetWindowThreadProcessId($candidate, [ref]$candidateProcessId)
      if ($candidateProcessId -eq $process.Id) {
        $className = [Text.StringBuilder]::new(256)
        [void][WifiOptSmokeNative]::GetClassName($candidate, $className, $className.Capacity)
        if ($className.ToString() -eq 'SmartWifiApOptimizerGuiWindow-v1') {
          $script:window = $candidate
        } elseif ($className.ToString() -eq 'SmartWifiApOptimizerDetailsWindow-v1') {
          $script:detailsWindow = $candidate
        }
      }
      return $true
    }
    [void][WifiOptSmokeNative]::EnumWindows($findWindow, [IntPtr]::Zero)
    $window = $script:window
    $detailsWindow = $script:detailsWindow
    if ($window -ne [IntPtr]::Zero -and $detailsWindow -ne [IntPtr]::Zero) { break }
    if ($process.HasExited) { throw "GUI exited early with code $($process.ExitCode)" }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)

  if ($window -eq [IntPtr]::Zero) { throw 'Main GUI window was not found' }
  if ($detailsWindow -eq [IntPtr]::Zero) { throw 'AP details window was not created' }

  $children = [Collections.Generic.List[object]]::new()
  $enumerateChild = [WifiOptSmokeNative+EnumWindowsProc]{
    param([IntPtr]$child, [IntPtr]$parameter)
    $className = [Text.StringBuilder]::new(256)
    $text = [Text.StringBuilder]::new(512)
    [void][WifiOptSmokeNative]::GetClassName($child, $className, $className.Capacity)
    [void][WifiOptSmokeNative]::GetWindowText($child, $text, $text.Capacity)
    $children.Add([pscustomobject]@{
      Class = $className.ToString()
      Id = [WifiOptSmokeNative]::GetDlgCtrlID($child)
      Text = $text.ToString()
    })
    return $true
  }
  [void][WifiOptSmokeNative]::EnumChildWindows($window, $enumerateChild, [IntPtr]::Zero)

  $combo = $children | Where-Object { $_.Class -eq 'ComboBox' -and $_.Id -eq 1008 }
  $apply = $children | Where-Object { $_.Class -eq 'Button' -and $_.Id -eq 1009 -and $_.Text -eq 'Apply' }
  $details = $children | Where-Object { $_.Class -eq 'Button' -and $_.Id -eq 1011 -and $_.Text -eq 'Details' }
  if (-not $combo) { throw 'Target SSID ComboBox (ID 1008) was not found' }
  if (-not $apply) { throw 'Apply button (ID 1009) was not found' }
  if (-not $details) { throw 'Details button (ID 1011) was not found' }

  $detailsChildren = [Collections.Generic.List[object]]::new()
  $enumerateDetailsChild = [WifiOptSmokeNative+EnumWindowsProc]{
    param([IntPtr]$child, [IntPtr]$parameter)
    $className = [Text.StringBuilder]::new(256)
    [void][WifiOptSmokeNative]::GetClassName($child, $className, $className.Capacity)
    $detailsChildren.Add([pscustomobject]@{
      Class = $className.ToString()
      Id = [WifiOptSmokeNative]::GetDlgCtrlID($child)
    })
    return $true
  }
  [void][WifiOptSmokeNative]::EnumChildWindows($detailsWindow, $enumerateDetailsChild, [IntPtr]::Zero)
  $detailsText = $detailsChildren | Where-Object { $_.Class -eq 'Edit' -and $_.Id -eq 1012 }
  if (-not $detailsText) { throw 'AP details read-only text control (ID 1012) was not found' }

  [void][WifiOptSmokeNative]::SendMessage($window, 0x0111, [IntPtr]1006, [IntPtr]::Zero)
  if (-not $process.WaitForExit(15000)) { throw 'GUI did not exit within 15 seconds' }
  if ($process.ExitCode -ne 0) { throw "GUI exited with code $($process.ExitCode)" }

  "PID=$($process.Id) WINDOW=$window"
  "COMBOBOX_ID=$($combo.Id) CLASS=$($combo.Class) TEXT=$($combo.Text)"
  "APPLY_ID=$($apply.Id) CLASS=$($apply.Class) TEXT=$($apply.Text)"
  "DETAILS_ID=$($details.Id) CLASS=$($details.Class) TEXT=$($details.Text)"
  "DETAILS_WINDOW=$detailsWindow DETAILS_TEXT_ID=$($detailsText.Id) CLASS=$($detailsText.Class)"
  "EXIT_CODE=$($process.ExitCode)"
} finally {
  if ($null -ne $process -and -not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
  }
  if ($null -eq $previousDataDirectory) {
    Remove-Item Env:WIFIOPT_DATA_DIR -ErrorAction SilentlyContinue
  } else {
    $env:WIFIOPT_DATA_DIR = $previousDataDirectory
  }
}
