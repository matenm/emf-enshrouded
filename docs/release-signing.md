# EMF.exe and code signing

EMF.exe is not code-signed. An unsigned program downloaded from the internet makes Windows SmartScreen show "Windows
protected your PC" until the program has built a reputation. The player clicks "More info", then "Run anyway". EMF's
DLLs do not trigger this; only EMF.exe does, and only when it is started.

## What EMF does about it (free)

- **Two downloads on Nexus:**
  - "EMF <version>" (with EMF.exe);
  - "EMF <version> Manual" (no .exe: `install.cmd` is a script, and the overlay's Mods tab does switches and settings).

  A player who doesn't want to run an unsigned exe never has to.
- **The Nexus page says it:** what the warning is, that EMF.exe is optional, and how to check the download (its
  SHA-256 in the release notes).
- **Each release is submitted to Microsoft:** https://www.microsoft.com/wdsi/filesubmission ("Software developer",
  "Incorrectly detected as malware/malicious"). This helps both antivirus false positives and reputation. Upload
  EMF.exe and the zip.
- **Stable identity:** the same file name, the same version info and the same publisher name in the version resource
  (`app/app.rc`: CompanyName "EMF project", ProductName, OriginalFilename) help reputation carry over between versions.

## Free signing for open-source projects

- **SignPath Foundation** (https://signpath.org) signs open-source projects for free. It requires:
  - a public repository with an OSI licence;
  - releases built on a CI service (GitHub Actions);
  - the signing request made from that build.

  EMF's release zips are made with `scripts\package.ps1`, not on a CI service, so EMF.exe is not signed this way.

## Not chosen

- A paid OV/EV certificate (a yearly fee).
- Azure Trusted Signing (a monthly fee).
- Microsoft Store packaging (a different install model; the game folder is outside the Store's reach).
