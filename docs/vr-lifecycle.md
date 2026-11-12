# VR lifecycle requirements

`VrLifecycle` coordinates the existing presentation and tracking policies. The
OpenVR shell supplies runtime observations and effects. Input state, rendering,
pose geometry and independent watchdogs retain their existing owners.

The host acceptance scenarios exercise these requirements through real App input
and private receivers. They do not require a particular list of internal states
or callbacks.

1. Changing the typing target releases held keys to the old receiver and discards
   queued input before the new receiver is selected. This applies even when both
   receivers use the same transport. An unchanged target must not interrupt a hold.
   A change that keeps the target, such as moving the panel, a pointer switch,
   Settings or a recenter, releases holds as if let go and still delivers text
   already accepted for that target, in order.
2. A hidden dashboard or panel cannot deliver pointer input, repeats or queued
   text. Hiding cancels movement and pointer captures. Reopening permits fresh
   input, without replaying the cancelled work, and shows no hover fade, swipe
   trail or key easing left from before.
3. Closing the panel dismisses the current request while the process remains
   resident. Late status for that request cannot reopen or acknowledge it. A
   fresh request may open the panel. Choosing native fallback has the same
   request isolation but asks Steam to preserve native input rather than dismiss.
   Steam's keyboard serving a field Full Keyboard cannot type into, or the
   helper lost during a request, hides a panel that Focus mode or dictation
   holds: Focus mode's waits for the next request, and dictation's returns, with
   its text, once Steam's keyboard closes.
4. A request is acknowledged only after successful presentation with usable
   placement/tracking, an open dashboard, actual compositor visibility and a
   ready input backend. Replacement typing waits for takeover acknowledgement.
   A failed presentation cannot consume a pending show or advertise success.
5. Recenter/show requests survive temporarily closed dashboards and missing
   tracking. Tracking recovery restores readiness without reviving a dismissed
   or expired request. Background startup stays hidden until requested.
6. The panel fades in over 0.18 s while rising 2 cm into place, and fades out
   over 0.12 s while settling 1 cm. Only the overlay's opacity and position
   change, so no frame is repainted. The entrance starts once the first image
   is submitted, and its end is written even if no frame fell inside it, so a
   slow first frame cannot leave the panel transparent. Hiding ends input at once; the fading panel
   takes none. Native fallback, the stall watchdog and shutdown hide it at once,
   so it never lingers over Steam's own keyboard.
7. Focus or tracking-space loss cancels input and movement. Losing one pointer
   device releases that device's keys without releasing the other hand. A lost
   movement owner ends movement; an unrelated device loss or laser leave does
   not end a valid movement capture.
8. Headset standby and system suspend cancel held input and queued output
   before any wake event is handled, even while tracking stays valid. Nothing
   pressed before sleep restarts after it; unsent dictation text stays in the
   editor.
9. While Steam's dashboard page has the keys, no key reaches the app behind
   it, until a trigger pull off the keyboard leaves an app with the keys,
   another app takes focus, or the dashboard closes. A pull on the dashboard's
   bar or in an app window chooses that app only while its window is in sight.

These checks complement the existing output, bridge, movement and tracking tests.
They cannot establish physical OpenVR presentation or controller behavior. Runtime
timings, watchdogs, frame pacing and independent native recovery remain unchanged.
