# Linux Hello Camera

Unlock your Linux PC with your face, like Windows Hello: at the login screen, the lock screen,
`sudo`, and admin password dialogs.

It uses your laptop's **IR camera** and a small face engine, and comes with a desktop app for
GNOME and KDE Plasma, so you don't need the terminal.

## What you need

- An Arch-based Linux distribution (CachyOS, Arch, EndeavourOS, …)
- A laptop or webcam with an **IR camera** (the kind that works with Windows Hello)
- **GNOME** or **KDE Plasma** (the app runs on other desktops too, but only has login switches for these two)

## Install

```bash
git clone https://github.com/ducvd89/linux-hello-camera.git
cd linux-hello-camera
./install.sh
```

Enter your password when asked. The first install takes a few minutes: it builds the face engine
and downloads its face models.

It installs the engine and the app and picks your IR camera automatically.
Face unlock isn't turned on anywhere until you do it in the app.

## Set up face unlock

Open **Linux Hello Camera** from your app grid.

1. **Add your face**: on the *Face Unlock* tab, click **Add** and look at the camera. It takes
   5 pictures.
2. **Test it**: click **Test**. You should see *Face recognised*.
3. **Try it with sudo**: turn on **sudo**, open a new terminal and run `sudo true`.
4. **Turn on the rest**: turn on the login switch, the lock screen switch (on KDE) and
   **Administrator prompts**. The login switch covers whichever login screen you have: GDM, SDDM
   or Plasma Login.
   - **GNOME:** one switch, *Login and lock screen*, because GNOME uses the same setting for both
   - **KDE Plasma:** *Login screen* and *Lock screen*

   On KDE Plasma, the **lock screen** looks for your face when you **press any key or click**
   (moving the mouse doesn't count, so locking the screen won't unlock it again straight away),
   and you can still type your password while it looks. The **login screen** (SDDM or Plasma
   Login) only checks after you press **Enter** with the password field empty. That's how those
   login screens work, not a bug.

5. **Confirm like Windows Hello** (on by default): when sudo or an administrator prompt asks
   for authentication, a small window opens straight away saying *Making sure it's you*, so you
   know to look at the camera. Once your face is recognised, **Continue** lights up; click it (or
   press Enter) to approve, so nothing gets admin rights just because you happened to be looking
   at the screen. It cancels itself after 15 seconds (you can change that in *Settings*). The
   lock and login screens don't ask. You can turn this off with **Confirm sudo and admin
   prompts**.

Your password always still works. If it doesn't recognise you within a few seconds, a message
says *Couldn't recognise you* (for sudo and admin prompts) and you just type your password as usual.

### If something isn't right

- **Wrong camera, or no face found**: on the *Camera* tab, click **Preview**, pick the camera
  where you can see yourself (usually called *IR camera*), then click **Use This Camera**.
- **Not recognised often**: add your face again, for example with and without glasses or in
  different lighting. You can also lower the *Match threshold* in *Settings* a little.
- **"Looks like a photo or screen" for your own face**: the check for a real face needs the
  camera's IR light to flash. On the *Camera* tab, the preview says whether it does. If your
  camera doesn't flash, turn off *Check for a real face using the IR light* in *Settings*.
- **Recognises you too slowly**: see *Settings* → *Timeout* and *Model* (*EdgeFace XS* is faster).

## Uninstall

```bash
./install.sh --uninstall
```

This turns face unlock off everywhere and removes the package. Your stored face templates stay in
`/var/lib/linux-hello-camera` until you delete that folder.

## Good to know

- **Face unlock is convenient, not more secure.** Someone who looks like you could get in.
  Always keep a password on your account.
- **Your face is stored as templates, not photos.** A template is a list of numbers that
  can't be turned back into a picture of you. If your PC has a TPM 2.0 chip and Secure Boot is on,
  the templates are also encrypted and locked to this PC's security chip *and* to Secure Boot, so
  copying them to another machine, or booting your laptop from a USB stick, is useless. The price:
  if your Secure Boot keys change, you have to add your face again (your password works meanwhile).
  With a TPM but Secure Boot off, this is off by default; you can turn it on with **Lock face data
  to this PC's security chip** on the *Face Unlock* tab (TPM only then). Without a TPM the switch
  is greyed out and the templates are stored without encryption. Encrypting your whole disk is
  still the answer for a stolen laptop.
- If you **log in** with your face (rather than just unlocking), your password store stays locked
  (GNOME Keyring or KDE Wallet), so you may be asked for your password once.
- Installer options: `./install.sh --help`.
- How it works and what it changes on your system: [app/README.md](app/README.md). The contract
  between the engine and the app: [DESIGN.md](DESIGN.md).

## Credits and license

MIT. The face engine in `engine/` is based on [biopass](https://github.com/TickLabVN/biopass)
(MIT, TickLab), and uses its face models. This project started as a fork of
[Howdy](https://github.com/boltgolt/howdy) by boltgolt; no Howdy code remains.
