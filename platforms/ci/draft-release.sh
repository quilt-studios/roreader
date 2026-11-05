#!/bin/bash
# Tag jobs assemble a draft only after every package and source job succeeds.
set -euo pipefail
version=${KOBALT_RELEASE_VERSION:?}
[[ ${GITHUB_REF:-} == "refs/tags/$version" ]] || { echo 'Draft releases require the matching tag push' >&2; exit 2; }
artifacts=(Kobalt-android-arm64.apk ROREADER-qt5.zip ROREADER-qt6.zip Kobalt-sources.tar.gz)
for name in "${artifacts[@]}"; do
    [[ -s build/release/$name ]] || { echo "Missing release artifact: $name" >&2; exit 1; }
done
(cd build/release && shasum -a 256 "${artifacts[@]}" build-*.json > SHA256SUMS)
if gh release view "$version" --json isDraft --jq .isDraft > build/release/draft-state.txt 2>build/release/draft-query.log; then
    [[ $(cat build/release/draft-state.txt) == true ]] || { echo 'Refusing to replace a published release' >&2; exit 1; }
else
    # A failed lookup may also be authentication or network failure. Creation
    # checks the tag server-side and must succeed before any upload occurs.
    prerelease=()
    [[ $version != *-* ]] || prerelease=(--prerelease)
    gh release create "$version" --verify-tag --draft "${prerelease[@]}" --title "ROREADER $version" --generate-notes
fi
rm -f build/release/draft-state.txt build/release/draft-query.log
uploads=()
for name in "${artifacts[@]}"; do uploads+=("build/release/$name"); done
gh release upload "$version" "${uploads[@]}" build/release/build-*.json build/release/SHA256SUMS --clobber
