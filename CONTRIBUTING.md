# Contributing

Bug reports, fixes and ideas are welcome.

## Reporting a bug

Open an issue with the bug form. Leave out anything that works as a key to your PC: the phone link, the QR code, the pairing code (the `t=` part of the link), and screenshots that show any of them. LocalCam's log doesn't contain the pairing code, so a log from `LocalCam.exe 2> log.txt` is fine to attach once you've read it through.

## Reporting a security problem

Please don't open a public issue. Report it privately at https://github.com/donerfolk/LocalCam/security/advisories/new.

## Building and testing

See [Build from source](README.md#build-from-source). `ctest --test-dir build -C Release` runs the self-checks; a fix to the server, the protocol or the video pipeline should come with a check that fails without it. [docs/PROTOCOL.md](docs/PROTOCOL.md) describes the wire protocol, so keep it in step with the code.

## Pull requests

Keep each pull request to one change, and say what it fixes and how you tested it: which phone, browser and video app.
