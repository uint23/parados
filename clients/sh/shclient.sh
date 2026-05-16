#!/bin/sh
#
# shclient
# A REPL client for parados

set -u

CACHE_LOGIN=${CACHE_LOGIN:-1}
VIDEO_PLAYER=${VIDEO_PLAYER:-mpv}
VIDEO_PLAYER_ARGS=${VIDEO_PLAYER_ARGS:-}
PARADOS_URL=${PARADOS_URL:-http://127.0.0.1:8088}
PARADOS_CURL=${PARADOS_CURL:-curl}

TAB=$(printf '\t')
CUR_DIR=""
AUTH=""
C_RESET=""
C_DIM=""
C_PROMPT=""
C_DIR=""
C_VID=""

CACHE_DIR=${XDG_CACHE_HOME:-"$HOME/.cache"}
AUTH_FILE="$CACHE_DIR/shclient.auth"

TMP_DIR=${TMPDIR:-/tmp}/shclient.$$
LIB_FILE="$TMP_DIR/library.tsv"
MAP_FILE="$TMP_DIR/view.tsv"

# cleanup on exit
cleanup() { rm -rf "$TMP_DIR"; }

# print err message, exit on failure
die() { printf '%s\n' "$*" >&2; exit 1; }

# check if command exists
cmd_exists() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1"; }

# setup subtle ANSI colors for interactive terminal output
setup_color()
{
	[ -t 1 ] || return 0
	[ -z "${NO_COLOR:-}" ] || return 0

	C_RESET=$(printf '\033[0m')
	C_DIM=$(printf '\033[2m')
	C_PROMPT=$(printf '\033[33m')
	C_DIR=$(printf '\033[36m')
	C_VID=$(printf '\033[32m')
}

# make sure arg isnt negative
validate_idx()
{
	case "$1" in
		''|*[!0-9]*) return 1 ;;
		*) return 0 ;;
	esac
}

# make sure arg is 16Byte hex
validate_id()
{
	case "$1" in
		????????????????)
			case "$1" in *[!0-9a-fA-F]*) return 1 ;; *) return 0 ;; esac
			;;
		*) return 1 ;;
	esac
}

# check if path is video based on extension
is_video_path()
{
	p=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')
	case "$p" in
		*.mkv|*.mp4|*.webm|*.mov|*.m4v|*.avi|*.mpg|*.mpeg|*.ts|*.m2ts|*.flv|*.wmv|*.3gp) return 0 ;;
		*) return 1 ;;
	esac
}

# percent encode credentials to be safely embedded in the URL
urlencode_userinfo()
{
	LC_ALL=C awk -v s="$1" '
	BEGIN {
		safe = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~"
		for (i = 1; i < 256; i++)
			ord[sprintf("%c", i)] = i
		for (i = 1; i <= length(s); i++) {
			c = substr(s, i, 1)
			if (index(safe, c) > 0)
				printf "%s", c
			else
				printf "%%%02X", ord[c]
		}
	}'
}

# do a HTTP GET request, save body to file, return HTTP code
http_get()
{
	if [ -n "$AUTH" ]; then
		"$PARADOS_CURL" -q -sS -u "$AUTH" -o "$2" -w '%{http_code}' "${PARADOS_URL}$1"
	else
		"$PARADOS_CURL" -q -sS -o "$2" -w '%{http_code}' "${PARADOS_URL}$1"
	fi
}

# parse /library JSON response, print "id<TAB>path" lines
parse_library_json()
{
	LC_ALL=C awk '
	{
		s = $0
		while (match(s, /"id":"([^"]+)","path":"([^"]*)"/)) {
			chunk = substr(s, RSTART, RLENGTH)
			id = chunk; sub(/^"id":"/, "", id); sub(/","path":"[^"]*"$/, "", id)
			path = chunk; sub(/^"id":"[^"]+","path":"/, "", path); sub(/"$/, "", path)
			gsub(/\\"/, "\"", path); gsub(/\\t/, "\t", path); gsub(/\\r/, "", path); gsub(/\\n/, "", path)
			printf "%s\t%s\n", id, path
			s = substr(s, RSTART + RLENGTH)
		}
	}
	' "$1"
}

# fetch /library, parse, save to LIB_FILE
refresh_library()
{
	tmp="$TMP_DIR/library.json"
	code=$(http_get "/library" "$tmp") || { printf '%s\n' "request failed: /library" >&2; return 1; }
	case "$code" in
		200)
			parse_library_json "$tmp" | while IFS="$TAB" read -r id path; do
				validate_id "$id" || continue
				is_video_path "$path" || continue
				printf '%s\t%s\n' "$id" "$path"
			done | sort -f -t "$TAB" -k2,2 > "$LIB_FILE" || { printf '%s\n' "failed parsing /library response" >&2; return 1; }
			;;
		401) printf '%s\n' "unauthorized: use login" >&2; return 1 ;;
		*) printf '%s\n' "request failed: /library (HTTP $code)" >&2; return 1 ;;
	esac
	return 0
}

# build view of current dir, save to MAP_FILE with lines:
build_view()
{
	dirs_raw="$TMP_DIR/dirs.raw"; dirs_sorted="$TMP_DIR/dirs.sorted"
	files_raw="$TMP_DIR/files.raw"; files_sorted="$TMP_DIR/files.sorted"
	: > "$dirs_raw"; : > "$files_raw"; : > "$MAP_FILE"

	while IFS="$TAB" read -r id path; do
		[ -n "$id" ] || continue
		if [ -n "$CUR_DIR" ]; then
			prefix="$CUR_DIR/"
			case "$path" in "$prefix"*) rel=${path#"$prefix"} ;; *) continue ;; esac
		else
			rel=$path
		fi
		case "$rel" in
			*/*) printf '%s\n' "${rel%%/*}" >> "$dirs_raw" ;;
			*) printf '%s\t%s\t%s\n' "$id" "$rel" "$path" >> "$files_raw" ;;
		esac
	done < "$LIB_FILE"

	sort -fu "$dirs_raw" > "$dirs_sorted"
	sort -f -t "$TAB" -k2,2 "$files_raw" > "$files_sorted"

	n=1
	while IFS= read -r dir; do
		[ -n "$dir" ] || continue
		if [ -n "$CUR_DIR" ]; then full="$CUR_DIR/$dir"; else full="$dir"; fi
		printf '%s\tD\t-\t%s\t%s\n' "$n" "$dir" "$full" >> "$MAP_FILE"
		n=$((n + 1))
	done < "$dirs_sorted"

	while IFS="$TAB" read -r id name full; do
		[ -n "$id" ] || continue
		printf '%s\tF\t%s\t%s\t%s\n' "$n" "$id" "$name" "$full" >> "$MAP_FILE"
		n=$((n + 1))
	done < "$files_sorted"
}

# given ls index, print corresponding line from MAP_FILE
lookup_entry()
{
	awk -F "$TAB" -v i="$1" '$1 == i { print; found=1; exit } END { if (!found) exit 1 }' "$MAP_FILE"
}

# split MAP_FILE line into vars
split_entry()
{
	IFS="$TAB" read -r ENTRY_N ENTRY_TYP ENTRY_ID ENTRY_NAME ENTRY_FULL <<EOF2
$1
EOF2
}

# print command prompt
print_prompt()
{
	if [ -n "$CUR_DIR" ]; then
		printf '%sparados>%s %s[/%s]%s ' "$C_PROMPT" "$C_RESET" "$C_DIM" "$CUR_DIR" "$C_RESET"
	else
		printf '%sparados>%s ' "$C_PROMPT" "$C_RESET"
	fi
}

# output all commands
cmd_help()
{
	cat <<'EOF2'
Commands:
  help                 show this help table
  clear                clear screen
  ping                 call "/ping"
  login                prompt for username, password
  logout               clear login (and cache if its enabled)
  rescan               do server rescan + refresh local view
  ls                   list dirs/videos in current folder
  cd n                 enter directory by ls index
  cd ..                go up one directory
  cd /                 go to root
  watch n              play video at ls index using VIDEO_PLAYER
  url URL              set server URL
  pwd                  show current folder
  quit | exit          leave shclient
EOF2
}

# clear screen
cmd_clear() { command -v clear >/dev/null 2>&1 && clear || printf '\033[2J\033[H'; }

# call "/ping", print response or error
cmd_ping()
{
	tmp="$TMP_DIR/ping.txt"
	code=$(http_get "/ping" "$tmp") || { printf '%s\n' "request failed: /ping" >&2; return 1; }
	case "$code" in
		200) cat "$tmp"; printf '\n' ;;
		401) printf '%s\n' "unauthorized: use login" >&2; return 1 ;;
		*) printf '%s\n' "request failed: /ping (HTTP $code)" >&2; return 1 ;;
	esac
}

# list current dir based on MAP_FILE
cmd_ls()
{
	build_view
	[ -n "$CUR_DIR" ] && printf 'cwd: /%s\n' "$CUR_DIR" || printf 'cwd: /\n'
	[ -s "$MAP_FILE" ] || { printf '%s\n' "(empty)"; return 0; }
	while IFS="$TAB" read -r n typ id name full; do
		case "$typ" in
			D) printf '%3s  %s[dir]%s %s/\n' "$n" "$C_DIR" "$C_RESET" "$name" ;;
			F) printf '%3s  %s[vid]%s %s\n' "$n" "$C_VID" "$C_RESET" "$name" ;;
		esac
	done < "$MAP_FILE"
}

# print current dir
cmd_pwd() { [ -n "$CUR_DIR" ] && printf '/%s\n' "$CUR_DIR" || printf '/\n'; }

# set server URL and reload library from new endpoint
cmd_url()
{
	[ $# -eq 1 ] || { printf '%s\n' "usage: url <http://host:port>" >&2; return 1; }
	PARADOS_URL=$1
	CUR_DIR=""
	if refresh_library; then
		printf 'server: %s\n' "$PARADOS_URL"
	else
		printf 'server set to: %s\n' "$PARADOS_URL"
		return 1
	fi
}

# change directory based on ls index, .., or /
cmd_cd()
{
	[ $# -eq 1 ] || { printf '%s\n' "usage: cd <n|..|/>" >&2; return 1; }
	case "$1" in
		/) CUR_DIR=""; return 0 ;;
		..)
			[ -z "$CUR_DIR" ] && return 0
			case "$CUR_DIR" in */*) CUR_DIR=${CUR_DIR%/*} ;; *) CUR_DIR="" ;; esac
			return 0
			;;
	esac
	validate_idx "$1" || { printf '%s\n' "cd expects an ls index, '..', or '/'" >&2; return 1; }
	build_view
	line=$(lookup_entry "$1") || { printf '%s\n' "invalid index: $1" >&2; return 1; }
	split_entry "$line"
	[ "$ENTRY_TYP" = "D" ] || { printf '%s\n' "index is not a directory: $1" >&2; return 1; }
	CUR_DIR=$ENTRY_FULL
}

# build URL for "/stream/id", embedding auth if needed for seeking
build_stream_url()
{
	base=${PARADOS_URL%/}
	path="/stream/$1"

	[ -n "$AUTH" ] || {
		printf '%s%s' "$base" "$path"
		return 0
	}

	case "$AUTH" in
		*:)
			user=${AUTH%:}
			pass=""
			;;
		*:*)
			user=${AUTH%%:*}
			pass=${AUTH#*:}
			;;
		*)
			user=$AUTH
			pass=""
			;;
	esac

	case "$base" in
		*://*)
			user_enc=$(urlencode_userinfo "$user") || return 1
			pass_enc=$(urlencode_userinfo "$pass") || return 1
			scheme=${base%%://*}
			rest=${base#*://}
			printf '%s://%s:%s@%s%s' "$scheme" "$user_enc" "$pass_enc" "$rest" "$path"
			;;
		*)
			printf '%s%s' "$base" "$path"
			;;
	esac
}

# given ls index, get video id, launch VIDEO_PLAYER on streams URL
cmd_watch()
{
	[ $# -eq 1 ] || { printf '%s\n' "usage: watch <n>" >&2; return 1; }
	validate_idx "$1" || { printf '%s\n' "watch expects an ls index" >&2; return 1; }
	build_view
	line=$(lookup_entry "$1") || { printf '%s\n' "invalid index: $1" >&2; return 1; }
	split_entry "$line"
	[ "$ENTRY_TYP" = "F" ] || { printf '%s\n' "index is not a video: $1" >&2; return 1; }
	validate_id "$ENTRY_ID" || { printf '%s\n' "invalid id in index: $1" >&2; return 1; }
	cmd_exists "$VIDEO_PLAYER"
	stream_url=$(build_stream_url "$ENTRY_ID") || { printf '%s\n' "failed building stream URL" >&2; return 1; }
	printf 'watching: %s\n' "$ENTRY_NAME"
	# shellcheck disable=SC2086
	set -- "$VIDEO_PLAYER" $VIDEO_PLAYER_ARGS "$stream_url"
	"$@"
}

# call "/rescan", print response or error
cmd_rescan()
{
	tmp="$TMP_DIR/rescan.txt"
	code=$(http_get "/rescan" "$tmp") || { printf '%s\n' "request failed: /rescan" >&2; return 1; }
	case "$code" in
		200) printf '%s\n' "rescan: ok"; refresh_library ;;
		401) printf '%s\n' "unauthorized: use login" >&2; return 1 ;;
		403) printf '%s\n' "rescan forbidden: auth may be disabled server-side" >&2; return 1 ;;
		*) printf '%s\n' "request failed: /rescan (HTTP $code)" >&2; return 1 ;;
	esac
}

# prompt for password without echo, return input
prompt_password()
{
	printf 'pass: ' >&2
	if command -v stty >/dev/null 2>&1; then
		stty_state=$(stty -g 2>/dev/null || true)
		stty -echo 2>/dev/null || true
		IFS= read -r pass || return 1
		[ -n "$stty_state" ] && stty "$stty_state" 2>/dev/null || stty echo 2>/dev/null || true
		printf '\n' >&2
	else
		IFS= read -r pass || return 1
	fi
	printf '%s' "$pass"
}

# save authentication cache if enabled and AUTH set, with permissions 600
save_auth_cache()
{
	[ "$CACHE_LOGIN" = "1" ] || return 0
	[ -n "$AUTH" ] || return 0
	mkdir -p "$CACHE_DIR" || return 1
	(umask 077 && printf '%s' "$AUTH" > "$AUTH_FILE") || return 1
}

# load authentication cache if enabled and cache file exists, set AUTH
load_auth_cache()
{
	[ "$CACHE_LOGIN" = "1" ] || return 0
	[ -f "$AUTH_FILE" ] || return 0
	AUTH=$(tr -d '\r\n' < "$AUTH_FILE")
}

# delete authentication cache file if enabled
clear_auth_cache()
{
	[ "$CACHE_LOGIN" = "1" ] || return 0
	rm -f "$AUTH_FILE"
}

# prompt for username and password, set AUTH
cmd_login()
{
	printf 'user: '
	IFS= read -r user || return 1
	[ -n "$user" ] || { printf '%s\n' "empty username" >&2; return 1; }
	pass=$(prompt_password) || return 1
	AUTH="$user:$pass"
	if ! refresh_library; then AUTH=""; return 1; fi
	save_auth_cache || printf '%s\n' "warning: failed to write auth cache" >&2
	printf '%s\n' "login: ok"
}

# clear AUTH, delete auth cache, print result
cmd_logout() { AUTH=""; clear_auth_cache; printf '%s\n' "logout: ok"; }

# dispatch command based on first arg, pass rest as args.
# return 99 to signal quit, 0 for success, 1 for error
dispatch()
{
	cmd=$1
	shift || true
	case "$cmd" in
		help) cmd_help ;;
		clear) cmd_clear ;;
		ping) cmd_ping ;;
		login) cmd_login ;;
		logout) cmd_logout ;;
		rescan) cmd_rescan ;;
		ls) cmd_ls ;;
		cd) cmd_cd "$@" ;;
		watch) cmd_watch "$@" ;;
		url) cmd_url "$@" ;;
		pwd) cmd_pwd ;;
		quit|exit) return 99 ;;
		'') return 0 ;;
		*) printf '%s\n' "unknown command: $cmd (use: help)" >&2; return 1 ;;
	esac
}

main()
{
	trap cleanup EXIT INT TERM
	cmd_exists "$PARADOS_CURL"
	mkdir -p "$TMP_DIR" || die "failed creating temp dir: $TMP_DIR"
	: > "$LIB_FILE"; : > "$MAP_FILE"

	if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then cmd_help; exit 0; fi
	[ $# -eq 0 ] || die "usage: shclient"

	load_auth_cache
	if ! refresh_library; then printf '%s\n' "not logged in. use: login" >&2; fi

	printf '%s\n' "shclient: minimal video repl"
	printf 'server: %s\n' "$PARADOS_URL"
	printf 'player: %s %s\n' "$VIDEO_PLAYER" "$VIDEO_PLAYER_ARGS"
	cmd_help

	setup_color

	while :; do
		print_prompt
		if ! IFS= read -r line; then printf '\n'; break; fi
		[ -n "$line" ] || continue
		set -- $line
		cmd=${1:-}
		shift || true
		dispatch "$cmd" "$@"
		rc=$?
		[ "$rc" -eq 99 ] && break
	done
}

main "$@"

