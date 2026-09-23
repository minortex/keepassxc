# Wayland libei preload backend

This directory builds an independent `LD_PRELOAD` library. It does not modify
KeePassXC. The library intercepts the QtDBus generated-interface call for
`org.freedesktop.portal.RemoteDesktop.NotifyKeyboardKeysym`, obtains the
portal's EIS file descriptor with `ConnectToEIS`, and sends the key event with
libei. Intercepting `QDBusConnection::call()` alone is insufficient because
QtDBus resolves generated-interface calls inside its own library.

Build it with:

```sh
cmake -S tools/keepassxc-libei -B /tmp/keepassxc-libei-build
cmake --build /tmp/keepassxc-libei-build
```

Start KeePassXC with:

```sh
LD_PRELOAD=/tmp/keepassxc-libei-build/libkeepassxc-libei.so keepassxc
```

Enable diagnostic messages with:

```sh
KEEPASSXC_LIBEI_DEBUG=1 \
  LD_PRELOAD=/tmp/keepassxc-libei-build/libkeepassxc-libei.so keepassxc
```

Before each detected auto-type sequence, the backend runs
`fcitx5-remote -c`. A new sequence is detected after 1 second without input;
adjust this with `KEEPASSXC_LIBEI_INPUT_METHOD_REARM_GAP_MS`.

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
