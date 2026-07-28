# Runs a command with a hidden console so builds/tests launched from GUIs or
# agent harnesses stop flashing terminal windows: child processes (ctest's
# test executables, compiler invocations) inherit the hidden console, while
# all output streams live to the calling terminal and the exit code is
# preserved. Usage: pwsh tools/run_quiet.ps1 -- <command> [args...]

# Plain $args keeps binding trivial: every token after the script path is
# part of the command line, with an optional leading "--" separator.
[string[]]$Command = @($args)
if ($Command.Count -gt 0 -and $Command[0] -eq '--') {
    $Command = $Command[1..($Command.Count - 1)]
}
if ($Command.Count -eq 0) {
    [Console]::Error.WriteLine('run_quiet.ps1: no command given')
    exit 2
}

# Resolve the executable so ProcessStartInfo does not depend on shell lookup.
$resolved = Get-Command $Command[0] -ErrorAction SilentlyContinue
if ($null -eq $resolved) {
    [Console]::Error.WriteLine("run_quiet.ps1: command not found: $($Command[0])")
    exit 127
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $resolved.Source
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.WorkingDirectory = (Get-Location).Path
for ($i = 1; $i -lt $Command.Count; $i++) {
    $startInfo.ArgumentList.Add($Command[$i])
}

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo

# Stderr streams via an async handler while stdout is pumped synchronously;
# this pairing cannot deadlock on full pipe buffers.
$stderrHandler = Register-ObjectEvent -InputObject $process `
    -EventName ErrorDataReceived -Action {
    if ($null -ne $EventArgs.Data) {
        [Console]::Error.WriteLine($EventArgs.Data)
    }
}

try {
    if (-not $process.Start()) {
        [Console]::Error.WriteLine("run_quiet.ps1: failed to start $($startInfo.FileName)")
        exit 126
    }
    $process.BeginErrorReadLine()

    while ($null -ne ($line = $process.StandardOutput.ReadLine())) {
        [Console]::Out.WriteLine($line)
    }

    $process.WaitForExit()
    exit $process.ExitCode
}
finally {
    Unregister-Event -SourceIdentifier $stderrHandler.Name -ErrorAction SilentlyContinue
    $process.Dispose()
}
