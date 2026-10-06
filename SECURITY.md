# Security

PadDisplay lets a tablet **see your PC's screen and control its keyboard, mouse, touch, pen and game controller input**. Please read what that means before using it on a network you don't trust.

## How it is protected
- **Pairing PIN.** Every connection must send a 6-digit PIN, over Wi-Fi and over USB alike. The PIN is generated per PC and shown in the tray menu. USB is included because the `adb reverse` tunnel is reachable by any app on the tablet, not just PadDisplay. You can turn it off for USB with `requireUsbPin=0` in `settings.ini`, at your own risk.
- **Rate limiting on wrong PINs.**
  - At most 5 attempts per minute from each address, and 20 per minute across all network addresses combined, then a 60 s lock-out.
  - Each failed attempt waits 1 s before the answer, and the PC shows a tray warning.
  - USB has its own limit, so someone on the network can't lock out a USB tablet.
- **The optional audio connection** uses the same PIN check and rate limits. It carries everything the PC plays, so it's only as private as the network, like the picture.
- **Handshakes run on their own threads with a 5 s timeout,** and at most 4 at a time, so an idle connection can't block the server.
- **Admin rights are needed only once,** for the driver install. The scheduled tasks it creates run `pnputil` with fixed arguments: two can only switch the virtual monitor device on or off, and a third (registered only if ViGEmBus is installed) can only switch the game controller driver on.

- **Administrator mode is off unless you turn it on** (tray → *Run as administrator*). PadDisplay normally runs with your normal rights, so the tablet can't control elevated windows. With it on, it can, and so can whoever has the PIN. If you also use *Start with Windows*, that registers a scheduled task (`PadDisplay Autostart`) that starts the exe with the highest rights when you sign in: keep the PadDisplay folder writable only by administrators, or someone who can replace the exe gets those rights.

- **Letting the tablet answer UAC prompts is off unless you turn it on** (tray → *Let the tablet answer UAC prompts in only-screen mode*, shown only when running as administrator). UAC prompts normally appear on the **secure desktop**, which can't be captured, so with the tablet as the PC's only screen they're invisible. With this on, while a tablet is connected as the only screen PadDisplay sets the machine-wide policy `PromptOnSecureDesktop = 0`, so prompts appear on the normal desktop and the tablet can show and click them. **This weakens UAC for the whole PC while it is active:** every UAC prompt, not just ones you trigger from the tablet, then appears on the ordinary desktop where other software could try to see or click it. PadDisplay restores the previous value the moment the tablet disconnects, when it exits, and at the next start-up if it was killed first (the old value is kept in `secure-desktop.txt`), so a crash won't leave the secure desktop disabled. Leave it off unless you need to click UAC prompts from the tablet.

## Known limitations
- **The stream is not encrypted.** Screen content and input travel as plain TCP, so anyone who can capture traffic on your network can see them. That includes everything typed on a keyboard connected to the tablet, passwords too.
- **Whoever has the PIN can type on your PC,** not only move the pointer: a paired tablet's keys are injected as real keystrokes. Use USB, or a network you trust. Encryption (TLS) is a welcome contribution.
- **The PIN is short.** With the rate limits above, guessing it takes on average weeks of continuous attempts, and the tray warns you when it happens. Don't expose port 27183 to the internet.
- **Other Windows users on the same PC** can reach the local port. On a shared PC, keep the USB PIN switched on.

## Reporting a vulnerability
Please **don't open a public issue** for security problems. Use GitHub's private vulnerability reporting instead (repository → *Security* → *Report a vulnerability*), with steps to reproduce. I'll respond as soon as I can.
