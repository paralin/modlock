## Install

You need one file: the `modlock` command for your computer. The quickest way is to let the installer pick it.

**Windows**, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/paralin/modlock/master/install.ps1 | iex
```

**macOS or Linux**, in a terminal:

```sh
curl -fsSL https://raw.githubusercontent.com/paralin/modlock/master/install.sh | sh
```

**With npm**, on any of them: `npm install --global @modlock/modlock`

Then run `modlock new my-mod`. The [README](https://github.com/paralin/modlock#getting-started) continues from there.

To download by hand instead, take the file for your computer from the assets below:

| Your computer               | File                        |
| --------------------------- | --------------------------- |
| Windows                     | `modlock-windows-amd64.exe` |
| Mac with Apple silicon      | `modlock-darwin-arm64`      |
| Mac with an Intel processor | `modlock-darwin-amd64`      |
| Linux, x86-64               | `modlock-linux-amd64`       |
| Linux, ARM64                | `modlock-linux-arm64`       |

Leave `modlock-host-windows-x64.zip` and the source code archives alone. The zip is the game server, which `modlock` downloads itself the first time it needs it, on Linux too.

If you already have `modlock`, run `modlock update`.
