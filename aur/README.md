# Публикация в AUR

Здесь лежат два пакета: `clipling` (релиз по тегу) и `clipling-git` (ветка `main`).
В AUR каждый пакет — отдельный git-репозиторий, где лежат только `PKGBUILD` и `.SRCINFO`.

## Один раз

1. Аккаунт на https://aur.archlinux.org, в профиле — публичный SSH-ключ.
2. `~/.ssh/config`:
   ```
   Host aur.archlinux.org
     IdentityFile ~/.ssh/aur
     User aur
   ```

## Новый релиз (`clipling`)

```bash
# в репозитории clipling: поднять version в meson.build и pkgver в PKGBUILD, закоммитить
git tag -a vX.Y.Z -m "clipling X.Y.Z" && git push origin main --tags

cd aur/clipling
updpkgsums                         # sha256 тарбола с GitHub вместо SKIP
makepkg -f && namcap PKGBUILD *.pkg.tar.zst   # проверка сборки
makepkg --printsrcinfo > .SRCINFO
```

Первая публикация:

```bash
git clone ssh://aur@aur.archlinux.org/clipling.git /tmp/aur-clipling
cp PKGBUILD .SRCINFO /tmp/aur-clipling/
cd /tmp/aur-clipling && git add PKGBUILD .SRCINFO && git commit -m "clipling X.Y.Z" && git push
```

Дальше — тот же клон: скопировать обновлённые файлы, коммит, push.

## `clipling-git`

Публикуется так же (`ssh://aur@aur.archlinux.org/clipling-git.git`). Обновлять
его при каждом коммите не нужно — `pkgver()` считает версию при сборке; push нужен,
только когда меняются зависимости или сам PKGBUILD.
