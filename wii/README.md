# Your Wii's storage

Wii Recomp needs a Wii's own storage: its settings, the Wii Menu, and the certificates and tickets games check. That storage comes from your own console, never from this repository. Put two files here, made with BootMii on your Wii (BootMii > Backup NAND):

| File | What it is |
| --- | --- |
| `nand.bin` | The NAND backup. |
| `keys.bin` | Your console's keys. Not needed when they're at the end of `nand.bin`, as BootMii writes them. |

Then decrypt them once:

```sh
python3 recomp/tools/import_nand.py
```

That makes `wii/nand/`, the storage `wii-recomp` uses when no other folder is named. Run it again after replacing `nand.bin`.

A Wii storage folder that's already decrypted, such as Dolphin's NAND folder after its system update, works too: copy its contents to `wii/nand/`, or name the folder when starting `wii-recomp`.

Everything in this folder except this file is ignored by git. The backup and keys belong to one console and hold its owner's data: don't share them.
