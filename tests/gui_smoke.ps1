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
    public static extern int GetWindowLong(IntPtr window, int index);

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
$graphWindow = [IntPtr]::Zero
$comparisonWindow = [IntPtr]::Zero
$diagnosticsWindow = [IntPtr]::Zero

try {
  $env:WIFIOPT_DATA_DIR = $resolvedDataDirectory
  $process = Start-Process -FilePath $resolvedExecutable -WindowStyle Hidden -PassThru
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  do {
    $script:window = [IntPtr]::Zero
    $script:detailsWindow = [IntPtr]::Zero
    $script:graphWindow = [IntPtr]::Zero
    $script:comparisonWindow = [IntPtr]::Zero
    $script:diagnosticsWindow = [IntPtr]::Zero
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
        } elseif ($className.ToString() -eq 'SmartWifiApOptimizerGraphWindow-v1') {
          $script:graphWindow = $candidate
        } elseif ($className.ToString() -eq 'SmartWifiApOptimizerComparisonWindow-v1') {
          $script:comparisonWindow = $candidate
        } elseif ($className.ToString() -eq 'SmartWifiApOptimizerDiagnosticsWindow-v1') {
          $script:diagnosticsWindow = $candidate
        }
      }
      return $true
    }
    [void][WifiOptSmokeNative]::EnumWindows($findWindow, [IntPtr]::Zero)
    $window = $script:window
    $detailsWindow = $script:detailsWindow
    $graphWindow = $script:graphWindow
    $comparisonWindow = $script:comparisonWindow
    $diagnosticsWindow = $script:diagnosticsWindow
    if ($window -ne [IntPtr]::Zero -and $detailsWindow -ne [IntPtr]::Zero -and
        $graphWindow -ne [IntPtr]::Zero -and $comparisonWindow -ne [IntPtr]::Zero -and
        $diagnosticsWindow -ne [IntPtr]::Zero) { break }
    if ($process.HasExited) { throw "GUI exited early with code $($process.ExitCode)" }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)

  if ($window -eq [IntPtr]::Zero) { throw 'Main GUI window was not found' }
  if ($detailsWindow -eq [IntPtr]::Zero) { throw 'AP details window was not created' }
  if ($graphWindow -eq [IntPtr]::Zero) { throw 'Realtime graph window was not created' }
  if ($comparisonWindow -eq [IntPtr]::Zero) { throw 'AP comparison window was not created' }
  if ($diagnosticsWindow -eq [IntPtr]::Zero) { throw 'Diagnostics window was not created' }

  $children = [Collections.Generic.List[object]]::new()
  $enumerateChild = [WifiOptSmokeNative+EnumWindowsProc]{
    param([IntPtr]$child, [IntPtr]$parameter)
    $className = [Text.StringBuilder]::new(256)
    $text = [Text.StringBuilder]::new(512)
    [void][WifiOptSmokeNative]::GetClassName($child, $className, $className.Capacity)
    [void][WifiOptSmokeNative]::GetWindowText($child, $text, $text.Capacity)
    $children.Add([pscustomobject]@{
      Handle = $child
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
  $ranking = $children | Where-Object { $_.Class -eq 'SysListView32' -and $_.Id -eq 1002 }
  $graph = $children | Where-Object { $_.Class -eq 'Button' -and $_.Id -eq 1013 }
  $compare = $children | Where-Object { $_.Class -eq 'Button' -and $_.Id -eq 1014 }
  $diagnostics = $children | Where-Object { $_.Class -eq 'Button' -and $_.Id -eq 1015 }
  if (-not $combo) { throw 'Target SSID ComboBox (ID 1008) was not found' }
  if (-not $apply) { throw 'Apply button (ID 1009) was not found' }
  if (-not $details) { throw 'Details button (ID 1011) was not found' }
  if (-not $ranking) { throw 'Ranking ListView (ID 1002) was not found' }
  $rankingStyle = [WifiOptSmokeNative]::GetWindowLong($ranking.Handle, -16)
  if (($rankingStyle -band 0x0004) -ne 0) { throw 'Ranking ListView still has LVS_SINGLESEL' }
  if (-not $graph) { throw 'Graph button (ID 1013) was not found' }
  if (-not $compare) { throw 'Compare button (ID 1014) was not found' }
  if (-not $diagnostics) { throw 'Diagnostics button (ID 1015) was not found' }

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

  function Get-ChildControls([IntPtr]$parent) {
    $items = [Collections.Generic.List[object]]::new()
    $enumerator = [WifiOptSmokeNative+EnumWindowsProc]{
      param([IntPtr]$child, [IntPtr]$parameter)
      $className = [Text.StringBuilder]::new(256)
      [void][WifiOptSmokeNative]::GetClassName($child, $className, $className.Capacity)
      $items.Add([pscustomobject]@{
        Class = $className.ToString()
        Id = [WifiOptSmokeNative]::GetDlgCtrlID($child)
      })
      return $true
    }
    [void][WifiOptSmokeNative]::EnumChildWindows($parent, $enumerator, [IntPtr]::Zero)
    return $items
  }

  $graphChildren = Get-ChildControls $graphWindow
  if (-not ($graphChildren | Where-Object { $_.Class -eq 'ComboBox' -and $_.Id -eq 1016 })) {
    throw 'Graph period ComboBox (ID 1016) was not found'
  }
  if (-not ($graphChildren | Where-Object { $_.Class -eq 'Button' -and $_.Id -eq 1017 })) {
    throw 'Graph export button (ID 1017) was not found'
  }

  $comparisonChildren = Get-ChildControls $comparisonWindow
  if (-not ($comparisonChildren | Where-Object { $_.Class -eq 'Edit' -and $_.Id -eq 1018 })) {
    throw 'Comparison read-only text control (ID 1018) was not found'
  }

  $diagnosticsChildren = Get-ChildControls $diagnosticsWindow
  if (-not ($diagnosticsChildren | Where-Object { $_.Class -eq 'Edit' -and $_.Id -eq 1019 })) {
    throw 'Diagnostics read-only text control (ID 1019) was not found'
  }
  if (-not ($diagnosticsChildren | Where-Object { $_.Class -eq 'Button' -and $_.Id -eq 1020 })) {
    throw 'Diagnostics refresh button (ID 1020) was not found'
  }
  if (-not ($diagnosticsChildren | Where-Object { $_.Class -eq 'Button' -and $_.Id -eq 1021 })) {
    throw 'Diagnostics export button (ID 1021) was not found'
  }

  [void][WifiOptSmokeNative]::SendMessage($window, 0x0111, [IntPtr]1006, [IntPtr]::Zero)
  if (-not $process.WaitForExit(15000)) { throw 'GUI did not exit within 15 seconds' }
  if ($process.ExitCode -ne 0) { throw "GUI exited with code $($process.ExitCode)" }

  "PID=$($process.Id) WINDOW=$window"
  "COMBOBOX_ID=$($combo.Id) CLASS=$($combo.Class) TEXT=$($combo.Text)"
  "APPLY_ID=$($apply.Id) CLASS=$($apply.Class) TEXT=$($apply.Text)"
  "DETAILS_ID=$($details.Id) CLASS=$($details.Class) TEXT=$($details.Text)"
  "RANKING_ID=$($ranking.Id) CLASS=$($ranking.Class) MULTISELECT=true"
  "DETAILS_WINDOW=$detailsWindow DETAILS_TEXT_ID=$($detailsText.Id) CLASS=$($detailsText.Class)"
  "GRAPH_WINDOW=$graphWindow PERIOD_ID=1016 EXPORT_ID=1017"
  "COMPARISON_WINDOW=$comparisonWindow TEXT_ID=1018"
  "DIAGNOSTICS_WINDOW=$diagnosticsWindow TEXT_ID=1019 REFRESH_ID=1020 EXPORT_ID=1021"
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
