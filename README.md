# WaveShareStarterSeed

Minimal ESP-IDF starter for `Waveshare ESP32-S3-Touch-LCD-7` with stability guardrails.

## Includes
- ESP-IDF CMake scaffold (`esp32s3` target)
- `sdkconfig.defaults` tuned for this board
- `partitions.csv`
- `main/idf_component.yml` baseline dependencies
- Stability files:
  - `WS_esp32Screen_DemoRules.md`
  - `STABILITY_CHECKLIST.md`
  - `.github/copilot-instructions.md`
  - `.github/skills/esp32-idf-stability/SKILL.md`
  - `.github/prompts/stability-audit.prompt.md`
  - `.github/prompts/pre-change-safety-gate.prompt.md`
  - `.github/prompts/app-focus-seed.prompt.md`
- VS Code essentials (`.vscode/settings.json`, `tasks.json`, `extensions.json`)

## Quick Start (Manual)
1. Copy this folder to a new project directory.
2. Replace `__PROJECT_NAME__` token in `CMakeLists.txt` with your project name.
3. Run:
   - `idf.py set-target esp32s3`
   - `idf.py update-dependencies`
   - `idf.py build`

## Quick Start (Scripted)
From repo root, run:

```powershell
.\New-WaveShareProject.ps1
```

The script asks only for project name, then creates the project from this seed.

No additional prompts are shown after project name.

By default it does not run `idf.py` outside VS Code, so target selection stays aligned with the ESP-IDF extension workflow.

Optional non-interactive usage:

```powershell
.\New-WaveShareProject.ps1 -ProjectName "MyNewProject" -DestinationRoot "c:\ESP32_Projects" -OpenInVSCode -InitializeEspIdf
```

Optional CLI fallback (only if you explicitly want it):

```powershell
.\New-WaveShareProject.ps1 -ProjectName "MyNewProject" -DestinationRoot "c:\ESP32_Projects" -InitializeEspIdf -InitializeEspIdfCli -OpenInVSCode
```

## Optional: Create GitHub Repo Automatically
If `git` and GitHub CLI (`gh`) are installed and authenticated (`gh auth login`), the script can initialize a local repo and publish it.

Create a private repo in your personal account:

```powershell
.\New-WaveShareProject.ps1 -ProjectName "MyNewProject" -CreateGitHubRepo
```

Create a public repo:

```powershell
.\New-WaveShareProject.ps1 -ProjectName "MyNewProject" -CreateGitHubRepo -GitHubVisibility public
```

Create under an organization:

```powershell
.\New-WaveShareProject.ps1 -ProjectName "MyNewProject" -CreateGitHubRepo -GitHubOrganization "MyOrg"
```

Create and open the new repo page in your browser:

```powershell
.\New-WaveShareProject.ps1 -ProjectName "MyNewProject" -CreateGitHubRepo -OpenGitHubRepoInBrowser
```
