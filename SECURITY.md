# Security

PadDisplay lets a tablet **see your PC's screen and control its mouse, touch and pen input**. Please read what that means before using it on a network you don't trust.

## How it is protected
- **Pairing PIN.** Every connection must send a 6-digit PIN, over Wi-Fi and over USB alike. The PIN is generated per PC and shown in the tray menu. USB is included because the `adb reverse` tunnel is reachable by any app on the tablet, not just PadDisplay. You can turn it off for USB with `requireUsbPin=0` in `settings.ini`, at your own risk.
- **Rate limiting on wrong PINs.**
  - At most 5 attempts per minute from each address, and 20 per minute across all network addresses combined, then a 60 s lock-out.
  - Each failed attempt waits 1 s before the answer, and the PC shows a tray warning.
  - USB has its own limit, so someone on the network can't lock out a USB tablet.
- **The optional audio connection** uses the same PIN check and rate limits. It carries everything the PC plays, so it's only as private as the network, like the picture.
- **Handshakes run on their own threads with a 5 s timeout,** and at most 4 at a time, so an idle connection can't block the server.
- **Admin rights are needed only once,** for the driver install. The two scheduled tasks it creates run `pnputil` with fixed arguments and can only switch the virtual monitor device on or off.

## Known limitations
- **The stream is not encrypted.** Screen content and input travel as plain TCP, so anyone who can capture traffic on your network can see them. Use USB, or a network you trust. Encryption (TLS) is a welcome contribution.
- **The PIN is short.** With the rate limits above, guessing it takes on average weeks of continuous attempts, and the tray warns you when it happens. Don't expose port 27183 to the internet.
- **Other Windows users on the same PC** can reach the local port. On a shared PC, keep the USB PIN switched on.

## Reporting a vulnerability
Please **don't open a public issue** for security problems. Use GitHub's private vulnerability reporting instead (repository → *Security* → *Report a vulnerability*), with steps to reproduce. I'll respond as soon as I can.
