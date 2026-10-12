# Releasing

## Continuous integration

`.github/workflows/ci.yml` runs on every push to `main` and on pull requests:

- **Native core**: builds the core and tests with clang, AddressSanitizer and
  UndefinedBehaviorSanitizer, runs all unit and loopback tests, builds the
  libFuzzer targets and fuzzes each of them for 60 seconds.
- **Android**: lint (release variant), JVM unit tests, debug APKs and release APKs.
  The APKs are uploaded as a workflow artifact.

## Publishing a release

1. Update `CHANGELOG.md`.
2. Tag and push:

   ```
   git tag v0.2.0
   git push origin v0.2.0
   ```

`.github/workflows/release.yml` then builds the release with that version
(`versionName` from the tag, `versionCode` = major·10000 + minor·100 + patch), verifies
the signatures and publishes a GitHub release with:

- `AirPlayTV-v<version>-arm64.apk`, `-armv7.apk`, `-x86_64.apk`, `-universal.apk`
- `SHA256SUMS`
- release notes with install instructions, the changelog section and checksums

A tag with a suffix (`v0.3.0-rc.1`) is published as a pre-release. TVs offer it under
Software Update only when they run a test version themselves; everyone else gets the next
release.

A release can be rebuilt from an existing tag with *Run workflow* on the Release workflow.

## Updates on the TV

Advanced → Software Update reads `https://api.github.com/repos/<owner>/<name>/releases`,
where the repository is `besliky/airplay-tv` unless the app is built with
`-PupdateRepository=owner/name`. It needs the asset names this workflow produces
(`AirPlayTV-v<version>-<abi>.apk` and `SHA256SUMS`) and the release signing key: a
package signed with another key does not install over the app.

## Signing

Release APKs are signed with the key from these repository secrets:

| Secret | Content |
|---|---|
| `RELEASE_KEYSTORE_BASE64` | the keystore (`base64 -w0 release.jks`) |
| `RELEASE_KEYSTORE_PASSWORD` | keystore password |
| `RELEASE_KEY_ALIAS` | key alias |
| `RELEASE_KEY_PASSWORD` | key password |

Without them, the workflow still produces installable APKs signed with a temporary key and
prints a warning. Such APKs cannot update an installation signed with a different key.

Create a key once and keep a copy outside the repository:

```
keytool -genkeypair -v -keystore release.jks -alias airplaytv -keyalg RSA -keysize 4096 -validity 10000
gh secret set RELEASE_KEYSTORE_BASE64 < <(base64 -w0 release.jks)
gh secret set RELEASE_KEYSTORE_PASSWORD
gh secret set RELEASE_KEY_ALIAS
gh secret set RELEASE_KEY_PASSWORD
```

## Local builds

`./gradlew packageReleaseApks` writes the same file set to `app/build/dist`. Local release
builds use `keystore.properties` if present (see the README) and fall back to the debug
key, so they can be installed for testing right away.
