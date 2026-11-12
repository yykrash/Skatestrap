# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

Signed files: `Skatestrap.exe` (the launcher) and `loader/loader.exe`. They are built from this repository by
GitHub Actions ([.github/workflows/build.yml](.github/workflows/build.yml)); only builds of tagged releases are signed.
Files not built from this repository (the Roblox app a player downloads, the ANGLE DLLs, apkeep) are never signed
by this project.

## Team roles

- Committers and reviewers: [yykrash](https://github.com/yykrash)
- Approvers: [yykrash](https://github.com/yykrash)

Every signing request is approved by hand by an approver. All team members use multi-factor authentication on
GitHub and SignPath.

## Privacy policy

Skatestrap connects to other systems only for what it does:

- **Updates:** on start it downloads `https://krashscripts.xyz/update/manifest.json` and, when there is a new
  version, the Skatestrap files listed in it. No personal data is sent.
- **Roblox:** it downloads the Roblox Android app from a public app mirror on the player's own computer, and the
  game talks to Roblox's servers like the Roblox app does. Your Roblox sign-in is stored on your computer,
  encrypted with Windows DPAPI, and is only ever sent to roblox.com.
- **Logs (off by default):** if you turn on "Send run.log", the game log is uploaded to the Skatestrap server
  when the game closes, to help fix crashes. Logs are deleted after 14 days.

Nothing else is collected or sent.
