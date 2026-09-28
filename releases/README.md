# Stable listening reference

`VibeAutotune-1.0.3-stable.component` is the Audio Unit binary that was
listening-tested as the most stable version during the prior work.

It is preserved here as a **golden audio reference**, not as reconstructible
source code. The Git history contains source only through version 1.0.2; the
source worktree used to build this 1.0.3 component was not committed and is no
longer available.

On another Mac, install this exact reference with:

```zsh
ditto "/path/to/autotune/releases/VibeAutotune-1.0.3-stable.component" \
      "$HOME/Library/Audio/Plug-Ins/Components/VibeAutotune.component"
```

Then open Logic's Plug-in Manager, run **Reset & Rescan Selection** for
VibeAutotune, and create a fresh plug-in instance. No `sudo` is needed.

Do not use the current uncommitted C++ source as a claim of source equivalence
with this 1.0.3 binary. Future changes must be compared audibly and with the
benchmark against this golden component/output.
