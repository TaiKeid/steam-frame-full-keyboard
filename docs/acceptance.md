# Acceptance checks

These are planned runtime checks, not claims of completed testing. Record actual results in the parent SteamFrame notes, including date, artifact, target machine and limitations.

## Visuals

- Full-size US arrangement, left Copy/Paste, separated navigation and numpad.
- Flat dark grey surface, white legends, rounded grey gradient keycaps.
- Shallow sides; fixed-size caps translate down on press without stretching or exposing a top side.
- Hover, press and modifier state remain distinguishable at actual headset size.
- Displayed legends and pointer hit regions align across supported scaling.

## Input matrix

Run against harmless test content, first in a dedicated receiver, then in Brave.

| Check | KDE Brave | Standalone floating Brave |
| --- | --- | --- |
| Main Enter and numpad Enter deliver key-down/up | Pending | Pending |
| Tab and Shift+Tab change focus correctly | Pending | Pending |
| Ctrl+A, Copy, Paste | Pending | Pending |
| Ctrl/Alt combined with letters on the same backend | Pending | Pending |
| Arrow/navigation/numpad keys | Pending | Pending |
| Focus survives a keyboard pointer click | Pending | Pending |
| No doubled characters from stock keyboard | Pending | Pending |
| Switching apps releases modifiers | Pending | Pending |

Test a form that submits on Enter and a multiline field that inserts a newline. The keyboard must emit Enter consistently; it cannot force arbitrary websites to implement form submission.

## Lifecycle and recovery

- Existing system keyboard button opens/closes the replacement without loops.
- Drag/reposition/scale follow the chosen panel behavior.
- Stock keyboard stays blocked only while the replacement is healthy.
- Cancelled pointer presses, tracking loss, focus loss and close release keys.
- Native crash restores stock UI and releases keys without relying on native shutdown code.
- Bridge failure, Steam restart, SteamVR restart, sleep/wake and reboot recover predictably.
- Uninstall restores previous configuration and preserves user data.
- Unsupported Steam API versions leave stock keyboard usable.

## Performance and privacy

Measure idle CPU, process memory, redraw time and pointer-to-feedback latency on Frame before setting budgets. Confirm no idle full-frame repaint loop and no texture-replacement flicker.

Typed text and clipboard contents must not appear in logs. Recovery must not paste or replay queued characters into a newly focused application.
