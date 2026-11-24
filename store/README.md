# Microsoft Store package

The Store re-signs the package with Microsoft's certificate, so Smart App Control trusts it
and the Store delivers updates. The same `VagiiMouse.exe` runs in both builds. When it runs
from a package (Store mode), the app:

- hides its own GitHub updater and "check for updates" toggle, and shows a button to the Store's updates page;
- uses the package StartupTask `VagiiMouseStartup` for "Start with Windows" instead of the
  HKCU Run key. It is off until the user turns it on, and starts hidden in the tray;
- skips the leftover cleanup that the self-updater needs;
- does the "Windows mouse registry" reset with Windows' own `reg.exe`, started through
  `cmd.exe` with the desktop-app breakaway policy, and saves the .reg backup to
  `Documents\Vagii Mouse`. Windows redirects a packaged app's own HKCU writes to a private
  copy, but system tools it starts run outside the package. Verified with
  `tools\probe-msix.ps1`, which only touches a throwaway key.

The first submission asked for `unvirtualizedResources` and was refused (policy 10.6.3).
The package no longer asks for it.

`VAGII_FAKE_STORE=1` shows Store mode in an unpackaged build (for screenshots).

## Build

```powershell
.\store\build-msix.ps1                       # bin\VagiiMouse-x.y.z.0-dev.msix, local test identity
.\store\build-msix.ps1 -Store -Name "<Identity Name>" -Publisher "CN=<...>" -PublisherDisplayName "<Store publisher name>"
```

Take the three values from Partner Center → your app → Product management → **Product identity**.
The store build goes to `bin\VagiiMouse-x.y.z.0-store.msix` and is uploaded unsigned.

## Local test

Windows only installs an unsigned package containing an exe in Developer Mode
(Settings → System → For developers). With Developer Mode on:

```powershell
Add-AppxPackage -Register .\obj\msix\AppxManifest.xml
```

## Submission (Partner Center)

1. Sign up at https://storedeveloper.microsoft.com with an individual account (free). Choose a
   publisher display name. It is public; your legal name is not.
2. Create the app with "Reserve a product name", for example **Vagii Mouse**.
3. Build with `-Store` and the Product identity values, then upload the .msix under Packages.
4. The one restricted capability, **runFullTrust**, needs a written reason in
   **Submission options**: a classic Win32 desktop app (Direct2D UI, tray icon, low-level mouse hook).
5. Store listing (text in `store/listing.md`):
   - description, at least one screenshot (1366×768 or larger), category Utilities & tools,
     age rating questionnaire;
   - privacy policy URL: the Store build makes no network calls, so a short statement
     page saying it collects nothing is enough.
   - **Support contact must not be a GitHub link** (rejected): use an email address or a
     non-GitHub website under Properties → Support info.
6. Pricing: Free. Submit. Certification usually takes 1–3 business days.
