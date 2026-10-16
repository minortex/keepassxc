# Wayland libei preload backend

This directory builds an independent `LD_PRELOAD` library. It does not modify
KeePassXC. The library intercepts the QtDBus generated-interface call for
`org.freedesktop.portal.RemoteDesktop.NotifyKeyboardKeysym`, obtains the
portal's EIS file descriptor with `ConnectToEIS`, and sends the key event with
libei. Intercepting `QDBusConnection::call()` alone is insufficient because
QtDBus resolves generated-interface calls inside its own library.
The original QtDBus functions are located at runtime by their demangled C++
signatures instead of hard-coded mangled names.

The backend picks a delivery path per auto-type sequence based on whether the
focused window is a native Wayland client or an X11/XWayland client. It samples
the focus once, at the first intercepted keysym, and reuses that decision for
the whole sequence (a sequence ends after `KEEPASSXC_LIBEI_INPUT_METHOD_REARM_GAP_MS`
without input, default 1 second).

For a **native Wayland** target the backend sends printable Unicode keysyms as
UTF-8 text through the EIS text device (libei 1.6 or newer). This allows
characters that are absent from the active keyboard layout to be typed, and the
input method is left untouched. Shortcut chords still use key events.

For an **X11/XWayland** target the backend keeps the keyboard keycode path and
disables the input method with `fcitx5-remote -c` before injecting, so the input
method does not swallow the injected keys.

## Target detection

XWayland keeps X input focus on a 1x1 dummy window (position -1,-1, no
`WM_CLASS`) whenever a native Wayland surface holds keyboard focus. When an X11
client is focused, `XGetInputFocus` returns that real window instead. The
backend opens the `DISPLAY` connection and inspects the focused window: a
missing `WM_CLASS` on a tiny window means a Wayland target, a real window with a
`WM_CLASS` means an X11 target. No compositor API is involved.

The main window is already hidden by the time the first keysym arrives (KeePassXC
hides it and waits `AutoTypeStartDelay` before typing), so the sampled focus has
settled on the target application.

## Known limitations

- **X11 non-ASCII characters are dropped.** EIS text cannot reach an X11 client:
  the compositor has to translate the text back into keycodes, and anything
  outside the active keymap is lost. This is a KWin limitation in the
  `forwardKeySym` fallback (temporary keymap around a scratch keycode), not a
  bug in this hook. Toggling the input method does not change it.
- **A pending input method preedit is committed, not discarded.**
  `fcitx5-remote -c` deactivates the input method, and fcitx commits whatever is
  in the preedit at that moment. The committed text ends up in the target field
  before the injected text. There is no `fcitx5-remote` option to clear a
  preedit.

Build it with:

```sh
cmake -S tools/keepassxc-libei -B /tmp/keepassxc-libei-build
cmake --build /tmp/keepassxc-libei-build
```

To exercise the portal and the preload hook without KeePassXC, run the test
sender with the same preload library:

```sh
KEEPASSXC_LIBEI_DEBUG=1 \
  LD_PRELOAD=/tmp/keepassxc-libei-build/libkeepassxc-libei.so \
  /tmp/keepassxc-libei-build/keepassxc-libei-test-autotype 'î¾ÙÇÕÁ'
```

Approve the RemoteDesktop request if prompted, then focus the target text field
within the 3-second countdown. The test sends each character through
`NotifyKeyboardKeysym`, so the preload hook follows the same interception and
EIS path as KeePassXC auto-type.

Start KeePassXC with:

```sh
LD_PRELOAD=/tmp/keepassxc-libei-build/libkeepassxc-libei.so keepassxc
```

Enable diagnostic messages with:

```sh
KEEPASSXC_LIBEI_DEBUG=1 \
  LD_PRELOAD=/tmp/keepassxc-libei-build/libkeepassxc-libei.so keepassxc
```

For an X11/XWayland target the backend runs `fcitx5-remote -c` before each
auto-type sequence. A new sequence is detected after 1 second without input;
adjust this with `KEEPASSXC_LIBEI_INPUT_METHOD_REARM_GAP_MS`. Set
`KEEPASSXC_LIBEI_KEEP_INPUT_METHOD` to skip this and leave the input method
untouched (useful for testing a native Wayland target while an input method is
active).

The test sender accepts `KEEPASSXC_LIBEI_TEST_DELAY_MS` to change the countdown
before typing (default 3000 ms), which gives more time to move focus.

`keepassxc-libei-x11-receiver` is a small Qt window used to test injection. Run
it with `QT_QPA_PLATFORM=xcb` for an X11 target (add `QT_IM_MODULE=fcitx` to give
it an input method) or with `QT_QPA_PLATFORM=wayland` for a native Wayland
target. It prints every received string to stdout.

KeePassXC normally runs as a single instance. Close every existing KeePassXC
process before starting it with `LD_PRELOAD`; otherwise the new process only
forwards the request to the old process, which does not contain this library.

`ConnectToEIS` is a portal v2 API. Once a `NotifyKeyboardKeysym` call is
intercepted, it is consumed even if EIS setup or key translation fails; it is
never sent through the keysym portal path as a fallback. The hook must be
installed before the process starts; it cannot retroactively alter an already
running process. When `WAYLAND_DISPLAY` is present and
`QT_QPA_PLATFORM` is unset, the preload library selects Qt's Wayland platform
so KeePassXC does not select the X11 auto-type backend under a Wayland session.
An explicitly set `QT_QPA_PLATFORM` is never overridden.
