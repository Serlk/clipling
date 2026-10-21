# Publishing to the AUR

There are two packages here: `clipling` (release, built from a tag) and
`clipling-git` (the `main` branch). In the AUR every package is its own git
repository that contains only `PKGBUILD` and `.SRCINFO`.

## One-time setup

1. An account on https://aur.archlinux.org with your public SSH key in the profile.
2. `~/.ssh/config`:
   ```
   Host aur.archlinux.org
     IdentityFile ~/.ssh/aur
     User aur
   ```

## New release (`clipling`)

```bash
# in the clipling repo: bump version in meson.build and pkgver in PKGBUILD, commit
git tag -a vX.Y.Z -m "clipling X.Y.Z" && git push origin main --tags

cd aur/clipling
updpkgsums                                    # sha256 of the GitHub tarball
makepkg -f && namcap PKGBUILD *.pkg.tar.zst   # check the build
makepkg --printsrcinfo > .SRCINFO
```

First publication:

```bash
git clone ssh://aur@aur.archlinux.org/clipling.git /tmp/aur-clipling
cp PKGBUILD .SRCINFO /tmp/aur-clipling/
cd /tmp/aur-clipling && git add PKGBUILD .SRCINFO && git commit -m "clipling X.Y.Z" && git push
```

After that, use the same clone: copy the updated files, commit, push.

## `clipling-git`

Published the same way (`ssh://aur@aur.archlinux.org/clipling-git.git`). It
does not need an update for every commit — `pkgver()` computes the version at
build time; push only when dependencies or the PKGBUILD itself change.
