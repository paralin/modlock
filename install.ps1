# Installs the modlock command line on Windows from the latest release.
#
#   irm https://raw.githubusercontent.com/paralin/modlock/master/install.ps1 | iex
#
# $env:MODLOCK_INSTALL_DIR chooses the directory, which defaults to
# %LOCALAPPDATA%\Programs\modlock. $env:MODLOCK_VERSION installs one release,
# such as v0.1.78, instead of the latest.

# The script block keeps these settings out of the terminal that runs it.
& {
  $ErrorActionPreference = 'Stop'
  # Invoke-WebRequest draws a progress bar that slows the download many times.
  $ProgressPreference = 'SilentlyContinue'

  $releases = 'https://github.com/paralin/modlock/releases'
  $asset = 'modlock-windows-amd64.exe'
  $url = if ($env:MODLOCK_VERSION) {
    "$releases/download/$env:MODLOCK_VERSION/$asset"
  } else {
    "$releases/latest/download/$asset"
  }

  $dir = if ($env:MODLOCK_INSTALL_DIR) {
    $env:MODLOCK_INSTALL_DIR
  } else {
    Join-Path $env:LOCALAPPDATA 'Programs\modlock'
  }
  New-Item -ItemType Directory -Force -Path $dir | Out-Null

  # Download beside the destination and rename into place, so an interrupted
  # download leaves any installed modlock working.
  $exe = Join-Path $dir 'modlock.exe'
  $staged = Join-Path $dir '.modlock.install.exe'
  Write-Host "Downloading $url"
  try {
    Invoke-WebRequest -Uri $url -OutFile $staged -UseBasicParsing
    Move-Item -Force -Path $staged -Destination $exe
  } finally {
    Remove-Item -Force -ErrorAction SilentlyContinue -Path $staged
  }

  # Put the directory on the user's PATH, for new terminals and for this one.
  $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
  if (($userPath -split ';') -notcontains $dir) {
    $entries = @($userPath, $dir) | Where-Object { $_ }
    [Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'User')
  }
  if (($env:Path -split ';') -notcontains $dir) {
    $env:Path = "$env:Path;$dir"
  }

  Write-Host "Installed $(& $exe --version) at $exe"
  Write-Host 'Start a mod with: modlock new my-mod'
}
