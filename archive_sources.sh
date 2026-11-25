#!/bin/sh
# tar.xz текущего Git-коммита без html/, .git и неотслеживаемых файлов; публикация на vc@vc.
# Запуск: ./archive_sources.sh [--no-publish] [OUTPUT.tar.xz]; локальный архив создаётся рядом с репозиторием.
set -eu

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

publish=yes
output=''
for arg do
    case "$arg" in
        --no-publish) publish=no ;;
        -h|--help)
            printf 'Usage: %s [--no-publish] [OUTPUT.tar.xz]\n' "$0"
            printf 'Archives HEAD without html/ using xz -9e; default: ../utun2-sources-COMMIT.tar.xz\n'
            printf 'Uploads to vc@vc:/home/vc/www/utun2-master.tar.xz and publishes HTML pages linking to it.\n'
            printf 'Use --no-publish to create only the local archive. Requires Git, xz; publication also requires SSH and SCP.\n'
            exit 0 ;;
        -*) fail "Unknown option: $arg; use --help" ;;
        *) [ -z "$output" ] || fail "Only one output path is allowed; use --help"; output=$arg ;;
    esac
done

command -v git >/dev/null 2>&1 || fail 'Git is required'
command -v xz >/dev/null 2>&1 || fail 'xz is required'
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || fail 'Cannot locate the repository'
commit=$(git -C "$root" rev-parse --verify 'HEAD^{commit}') || fail 'Cannot read the current Git commit'
short_commit=$(git -C "$root" rev-parse --short "$commit") || fail 'Cannot abbreviate the Git commit'
output=${output:-"$root/../utun2-sources-$short_commit.tar.xz"}
case "$output" in *.tar.xz) ;; *) fail 'Output path must end in .tar.xz' ;; esac
output_dir=$(CDPATH= cd -- "$(dirname -- "$output")" && pwd) || fail "Output directory does not exist: $output"
output="$output_dir/$(basename -- "$output")"
remote=vc@vc
site_dir=/home/vc/www
site_archive=utun2-master.tar.xz
if [ "$publish" = yes ]; then
    command -v scp >/dev/null 2>&1 || fail 'SCP is required for publication; use --no-publish for a local archive'
    command -v ssh >/dev/null 2>&1 || fail 'SSH is required for publication; use --no-publish for a local archive'
    grep -Fq "href=\"$site_archive\"" "$root/html/index.html" || fail "Source archive link not found in html/index.html"
fi

temporary=$(mktemp -d "$output.tmp.XXXXXX") || fail "Cannot create temporary directory beside $output"
trap 'rm -rf -- "$temporary"' 0
trap 'fail "Archive creation interrupted"' HUP INT TERM

printf 'INFO: Archiving commit %s without html/\n' "$commit"
git -C "$root" archive --format=tar --prefix=utun2/ --output="$temporary/sources.tar" "$commit" -- . ':(top,exclude)html' ||
    fail 'git archive failed; destination archive was not replaced'
xz -9e -T1 -c "$temporary/sources.tar" > "$temporary/sources.tar.xz" || fail 'xz failed; destination archive was not replaced'
mv -f -- "$temporary/sources.tar.xz" "$output" || fail "Cannot save the archive: $output"
printf 'INFO: Source archive ready: %s\n' "$output"

if [ "$publish" = yes ]; then
    remote_temporary="$site_dir/.$site_archive.$commit.tmp"
    printf 'INFO: Uploading source archive to %s:%s/%s\n' "$remote" "$site_dir" "$site_archive"
    scp -o BatchMode=yes -o ConnectTimeout=15 "$output" "$remote:$remote_temporary" || fail 'Source archive upload failed; pages were not published'
    ssh -o BatchMode=yes -o ConnectTimeout=15 "$remote" "mv -f -- $remote_temporary $site_dir/$site_archive" ||
        fail 'Cannot publish the uploaded source archive; pages were not published'
    for page in "$root"/html/*.html; do
        if grep -Fq "href=\"$site_archive\"" "$page"; then
            printf 'INFO: Publishing source download page: %s\n' "$(basename -- "$page")"
            scp -o BatchMode=yes -o ConnectTimeout=15 "$page" "$remote:$site_dir/" || fail "Cannot publish page: $page"
        fi
    done
    printf 'INFO: Source archive and download pages published: %s:%s/%s\n' "$remote" "$site_dir" "$site_archive"
fi
