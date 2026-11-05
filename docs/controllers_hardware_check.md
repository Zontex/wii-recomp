# Controllers on real hardware

Everything in the controllers stage is tested with stand-ins: SDL's virtual controllers, the loopback remote and made-up data through the virtual Bluetooth chip. This is the list for the day real controllers are in the room. Each step takes a few minutes. Someone holds the controllers; the commands and logs can be run and read from another machine.

Start every run with `WII_RECOMP_INPUT_STATS=1`. Every five seconds it prints a line for each controller:
- gyro samples a second
- how often the remote read its player
- how old the newest sample was when it did
- the gyro's zero

Keep those lines: they're the numbers this stage asks for.

## Raspberry Pi

**Setup, once.**
- Build SDL3 with `recomp/tools/sdl3_setup.sh`; `--prefix ~/.local` needs no root.
- Run `sudo recomp/tools/remotes_setup.sh pads`, then unplug and replug any controller.
- Real remotes and wireless Switch controllers don't work together yet. If the machine was set up for real remotes, `sudo recomp/tools/remotes_setup.sh --undo` first.

**Pair each wireless controller.**
1. Hold its sync button.
2. Run `bluetoothctl`, then `scan on`, `pair <address>`, `trust <address>`, `connect <address>`.

Switch 2 controllers go by USB cable: SDL 3.4 can't start them over Bluetooth.

**For each of the Pro Controller, the Joy-Con pair, one Joy-Con alone, the Switch 2 Pro Controller and Switch 2 Joy-Cons:**

1. **It connects.** Start Wii Fit Plus. The log says `controllers: <name> <serial> is player 1, with its gyro as the pointer`, and the controller's first player light comes on when the game lights the remote's.
2. **Pointer.** In the menus, the hand follows the controller.
   - Turning 20 degrees from the middle reaches the side of the screen.
   - It stays put when the controller is put down for a minute: check the gyro zero in the stats lines settles.
   - Clicking the right stick brings it back to the middle.
3. **Roll.** Turning the controller about its length turns the hand, and the hand stays where it points.
4. **Buttons.**
   - A is A, and B is both ZR and b.
   - With a Nunchuk plugged in (the default), the left stick moves through Wii Fit's menus where they take the stick.
   - F2 on the keyboard plugs in a Classic Controller for player 1; then the controller's buttons should be where a Classic Controller's are. Wii Fit hides its pointer then, which is expected.
5. **Body Test.** The scripted Body Test walk (`test/run.sh`) still passes with the controller connected, and its fps matches the keyboard run.
6. **Rumble.** Something in Wii Fit that rumbles the remote rumbles the controller, and stops.
7. **Battery.** The Home Menu shows the controller's battery level for P1.
8. **Coming and going.**
   - Switch the controller off mid-game: the log says it disconnected, and the game sees the remote leave.
   - Switch it on again: it joins as the same player, and the remote calls the game again.
9. **Joy-Con pair.** The right Joy-Con is the remote and the left the Nunchuk. Shaking the left one moves the Nunchuk's motion, which `WII_RECOMP_BLUETOOTH_LOG=1` shows in the report bytes.
10. **One Joy-Con alone.** It's a remote held sideways. The stick or buttons work the D-pad turned a quarter, and B and A are 1 and 2.

**Two players.** Connect the Pro Controller and the Joy-Con pair together. Each joins its own player, and Mario Kart Wii's Home Menu shows both. Pinning: set `source = pad:<serial>` for player 2 in `controllers.ini` and check the same controller comes back as player 2 after a restart.

**Record** for each controller the stats lines' gyro samples a second and the sample age at the median and the 99th percentile, and the Pi's fps against the keyboard run.

## Mac

Pair the same controllers in System Settings > Bluetooth, and allow Input Monitoring if macOS asks. Run steps 1 to 8 for each, and the two players. No timing on the Mac is needed.

## When they're in the room

- **Real Wii Remotes and a Balance Board on the Pi.**
  - Set up with `sudo recomp/tools/remotes_setup.sh`, and pair with `... pair` after pressing SYNC.
  - Check the report rate: about 100 a second, with `WII_RECOMP_BLUETOOTH_LOG=1`.
  - The board takes player 5's slot.
  - Remotes stay connected across the Wii Menu handing over to a disc.
- **A real Nunchuk on a real remote**, through Wii Fit's menus.
- **A game that uses Wii MotionPlus**, such as Wii Sports Resort.
  - Set `motionplus = yes` for player 1, with a Switch controller connected.
  - The game should activate the MotionPlus, pass its check, and follow the controller's turns.
  - It was built from Dolphin's description and tested with made-up data only, so this is its first real test.

Each thing that doesn't work is fixed in its own change and measured again.
