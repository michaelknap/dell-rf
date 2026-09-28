#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: packaging/release.sh [--publish]

Create or resume the signed release for the version in VERSION. By default,
the verified and signed release remains a draft. Use --publish to publish it.
EOF
}

die() {
    printf 'release: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

publish=false
case ${1:-} in
    '')
        ;;
    --publish)
        publish=true
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
[[ $# -le 1 ]] || {
    usage >&2
    exit 2
}

for command in git gh gpg make sha256sum; do
    require_command "$command"
done

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

version=$(<VERSION)
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
    die "VERSION is not a semantic version: $version"

tag="v$version"
debian_version=$(sed -n '1s/^dell-rf (\([^)]*\)).*/\1/p' debian/changelog)
[[ ${debian_version%-*} == "$version" ]] ||
    die 'VERSION and debian/changelog disagree'

[[ -z $(git status --porcelain --untracked-files=normal) ]] ||
    die 'the working tree is not clean'

current_branch=$(git symbolic-ref --quiet --short HEAD) ||
    die 'HEAD is detached'
remote_head=$(git symbolic-ref --quiet --short refs/remotes/origin/HEAD) ||
    die 'origin does not advertise a default branch'
default_branch=${remote_head#origin/}
[[ $current_branch == "$default_branch" ]] ||
    die "release must run from $default_branch, not $current_branch"

git fetch --quiet origin "$default_branch" --tags

head_sha=$(git rev-parse HEAD)
remote_sha=$(git rev-parse "origin/$default_branch")
[[ $head_sha == "$remote_sha" ]] ||
    die "HEAD must exactly match origin/$default_branch"

repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
[[ -n $repo ]] || die 'could not identify the GitHub repository'
gh auth status --hostname github.com >/dev/null

signing_key=$(git config --get user.signingkey || true)
[[ -n $signing_key ]] ||
    die 'configure git user.signingkey before releasing'
gpg --batch --list-secret-keys "$signing_key" >/dev/null 2>&1 ||
    die "secret signing key is unavailable: $signing_key"

printf 'Validating %s at %s.\n' "$tag" "$head_sha"
make clean
make check
make format-check

if git show-ref --verify --quiet "refs/tags/$tag"; then
    [[ $(git rev-list -n 1 "$tag") == "$head_sha" ]] ||
        die "$tag already points to another commit"
    git verify-tag "$tag"
else
    git tag --sign --local-user="$signing_key" \
        --message="dell-rf $version" "$tag" HEAD
    git verify-tag "$tag"
fi

remote_tag_object=''
if remote_tag_object=$(
    git ls-remote --exit-code --tags origin "refs/tags/$tag" |
        awk 'NR == 1 { print $1 }'
); then
    local_tag_object=$(git rev-parse "$tag^{tag}")
    [[ $remote_tag_object == "$local_tag_object" ]] ||
        die "origin already has a different $tag"
else
    status=$?
    [[ $status -eq 2 ]] || die "could not query $tag on origin"
    git push origin "refs/tags/$tag"
fi

printf 'Waiting for the release workflow.\n'
run_id=''
deadline=$((SECONDS + 180))
while [[ -z $run_id ]]; do
    if run_id=$(
        gh run list --repo "$repo" --workflow release.yml \
            --commit "$head_sha" --event push --limit 20 \
            --json databaseId,headBranch \
            --jq ".[] | select(.headBranch == \"$tag\") | .databaseId" |
            head -n 1
    ); then
        :
    else
        run_id=''
    fi

    [[ -n $run_id ]] && break
    ((SECONDS < deadline)) || die 'release workflow did not start within 3 minutes'
    sleep 3
done

gh run watch "$run_id" --repo "$repo" --compact --exit-status

is_draft=$(gh release view "$tag" --repo "$repo" --json isDraft --jq .isDraft)
[[ $is_draft == true ]] || die "$tag is already published"

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
gh release download "$tag" --repo "$repo" --dir "$work"

cd -- "$work"
source_asset="dell-rf-$version.tar.gz"
aur_asset="dell-rf-$version-aur.tar.gz"
arch_asset="dell-rf-$version-1-x86_64.pkg.tar.zst"
debian_asset="dell-rf_${version}-1_amd64.deb"
assets=("$source_asset" "$aur_asset" "$arch_asset" "$debian_asset")

[[ -f SHA256SUMS ]] || die 'draft release has no SHA256SUMS'
for asset in "${assets[@]}"; do
    [[ -f $asset ]] || die "draft release is missing $asset"
done

mapfile -t checksum_names < <(awk '{ sub(/^\*/, "", $2); print $2 }' SHA256SUMS | sort)
mapfile -t expected_names < <(printf '%s\n' "${assets[@]}" | sort)
[[ ${#checksum_names[@]} -eq ${#expected_names[@]} ]] ||
    die 'SHA256SUMS contains an unexpected number of files'
for index in "${!expected_names[@]}"; do
    [[ ${checksum_names[$index]} == "${expected_names[$index]}" ]] ||
        die 'SHA256SUMS does not contain exactly the expected release files'
done

sha256sum --check --strict SHA256SUMS

for asset in "${assets[@]}"; do
    gh attestation verify "$asset" \
        --repo "$repo" \
        --signer-workflow "$repo/.github/workflows/release.yml" \
        --source-ref "refs/tags/$tag" \
        --source-digest "$head_sha" \
        --deny-self-hosted-runners
done

rm -f -- SHA256SUMS.asc RELEASE-PUBLIC-KEY.asc RELEASE-KEY-FINGERPRINT
gpg --local-user "$signing_key" --armor --detach-sign \
    --output SHA256SUMS.asc SHA256SUMS
gpg --verify SHA256SUMS.asc SHA256SUMS

primary_fingerprint=$(
    gpg --batch --with-colons --fingerprint "$signing_key" |
        awk -F: '$1 == "fpr" { print $10; exit }'
)
[[ -n $primary_fingerprint ]] || die 'could not determine the primary fingerprint'
gpg --batch --armor --export "$primary_fingerprint" \
    > RELEASE-PUBLIC-KEY.asc
[[ -s RELEASE-PUBLIC-KEY.asc ]] || die 'could not export the release public key'
printf '%s\n' "$primary_fingerprint" > RELEASE-KEY-FINGERPRINT

gh release upload "$tag" --repo "$repo" --clobber \
    SHA256SUMS.asc RELEASE-PUBLIC-KEY.asc RELEASE-KEY-FINGERPRINT

if $publish; then
    gh release edit "$tag" --repo "$repo" --draft=false --latest
    printf 'Published %s.\n' "$tag"
else
    printf '%s is verified, signed and remains a draft.\n' "$tag"
    printf 'Publish it with: packaging/release.sh --publish\n'
fi
