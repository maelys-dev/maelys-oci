#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Contract tests of the maelys-oci terminal: catalog, envelopes, causal
# validation, plan/apply doctrine and dispatch through `maelys oci`.
# usage: tests/test_oci_cli.sh MAELYS_OCI MAELYS_DISPATCHER
set -eu
oci="$1"
maelys="$2"
work=$(mktemp -d "${TMPDIR:-/tmp}/maelys-oci-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
failures=0

check() {
    if eval "$2"; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s\n' "$1" >&2
        failures=$((failures + 1))
    fi
}

run() { # run NAME command...; captures $out $err $code
    shift
    set +e
    "$@" >"$work/out" 2>"$work/err"
    code=$?
    set -e
    out=$(cat "$work/out")
    err=$(cat "$work/err")
}

json_data() { # json_data FILE EXPRESSION: evaluates a Python expression on data
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); data=d.get("data"); print(eval(sys.argv[2]))' "$1" "$2"
}

expected_version=$(sed -n '1p' "$(dirname "$0")/../VERSION")
run version "$oci" --version
check "version matches the VERSION file" '[ "$code" = 0 ] && [ "$out" = "maelys-oci $expected_version" ] && [ -z "$err" ]'

run describe "$oci" describe --summary --format json --compact --non-interactive
check "describe summary is a silent envelope" '[ "$code" = 0 ] && [ -z "$err" ] && json_data "$work/out" "d[\"contract\"]" | grep -q "agent-cli/v2"'
check "describe lists every product command" '[ "$(json_data "$work/out" "sorted(c[\"id\"] for c in data[\"commands\"] if c[\"id\"] not in (\"help\",\"version\",\"describe\",\"completion\",\"complete.candidates\"))")" = "['"'"'gc'"'"', '"'"'import'"'"', '"'"'inspect'"'"', '"'"'list'"'"', '"'"'pull'"'"', '"'"'remove'"'"', '"'"'unpack-rootfs'"'"', '"'"'verify'"'"']" ]'

run describe-import "$oci" describe import --json --compact
check "import is a plan/apply transaction with a schema" 'json_data "$work/out" "data[\"commands\"][0][\"effect\"]" | grep -q "plan.*preview.*apply" && json_data "$work/out" "data[\"commands\"][0][\"outputSchema\"][\"required\"]" | grep -q "artifact"'

run describe-list "$oci" describe list --json --compact
check "list emits json-records" 'json_data "$work/out" "data[\"commands\"][0][\"outputMode\"]" | grep -q "json-records"'

run describe-pull "$oci" describe pull --json --compact
check "pull constrains --platform to a choice and declares the token/config conflict" 'json_data "$work/out" "[o for o in data[\"commands\"][0][\"input\"][\"options\"] if o[\"long\"] == \"--platform\"][0][\"argument\"][\"choices\"]" | grep -q "linux/arm64.*linux/amd64" && json_data "$work/out" "[o for o in data[\"commands\"][0][\"input\"][\"options\"] if o[\"long\"] == \"--token-file\"][0][\"conflictsWith\"]" | grep -q "docker-config"'

run help "$oci" help
check "help is generated from the catalog" '[ "$code" = 0 ] && printf "%s" "$out" | grep -q "import SOURCE \[--store DIRECTORY\]" && printf "%s" "$out" | grep -q "STORE"'

run unknown "$oci" inspect "$work" --loud --json --compact
check "unknown option is refused with an envelope on stderr" '[ "$code" = 1 ] && [ -z "$out" ] && printf "%s" "$err" | grep -q "\"code\":\"VALIDATION_FAILED\""'

run dry-run "$oci" gc --dry-run
check "legacy --dry-run is refused with the migration hint" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "Add --apply only after reviewing"'

run arity "$oci" inspect
check "missing operand is refused" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "VALIDATION_FAILED"'

run platform "$oci" pull registry.example/tool@sha256:0000000000000000000000000000000000000000000000000000000000000000 --platform linux/riscv64 --json --compact
check "pull refuses a platform outside the choice" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"VALIDATION_FAILED\""'

run conflict "$oci" pull registry.example/tool@sha256:0000000000000000000000000000000000000000000000000000000000000000 --token-file /a --docker-config /b --json --compact
check "pull refuses --token-file with --docker-config" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "VALIDATION_FAILED"'

run digest "$oci" import "$work" --digest nope --json --compact
check "import refuses a malformed --digest before touching the source" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"VALIDATION_FAILED\"" && printf "%s" "$err" | grep -q "sha256"'

run relative "$oci" verify --store relative/store --json --compact
check "relative --store is refused by the absolute-path kind" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"VALIDATION_FAILED\"" && printf "%s" "$err" | grep -qi "absolute"'

run relative-ca "$oci" pull registry.example/tool@sha256:0000000000000000000000000000000000000000000000000000000000000000 --ca-file certs/ca.pem --json --compact
check "relative --ca-file is refused before any network access" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"VALIDATION_FAILED\""'

run digest-kind "$oci" import "$work" --digest sha512:00 --json --compact
check "digest kind refuses an undeclared algorithm" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"VALIDATION_FAILED\""'

run describe-gc "$oci" describe gc --json --compact
check "typed default of --grace-seconds comes from the library constant" 'printf "%s" "$out" | grep -q "86400"'

run describe-unpack "$oci" describe unpack-rootfs --json --compact
if [ "$(uname -s)" = Linux ]; then
    check "unpack-rootfs is available on Linux" 'printf "%s" "$out" | grep -q "\"available\":true"'
else
    check "unpack-rootfs is described as unavailable outside Linux" 'printf "%s" "$out" | grep -q "\"available\":false" && printf "%s" "$out" | grep -q "unavailableReason"'
    run unavailable "$oci" unpack-rootfs /a.tar /b --json --compact
    check "unavailable command fails with UNSUPPORTED" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"UNSUPPORTED\""'
fi

run env-format env MAELYS_CLI_FORMAT=json "$oci" verify --store "$work/absent"
check "MAELYS_CLI_FORMAT=json selects the envelope without a flag" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\": \"PRECONDITION_FAILED\""'

run completion "$oci" completion bash
check "bash completion shim is generated from the catalog" '[ "$code" = 0 ] && printf "%s" "$out" | grep -q "_maelys_oci_complete maelys-oci"'

run complete-platform "$oci" __complete -- pull --platform ""
check "completion offers the platform choices" '[ "$out" = "linux/arm64
linux/amd64" ]'

run complete-commands "$oci" __complete -- ""
check "completion offers the product commands" 'printf "%s\n" "$out" | grep -qx "import" && printf "%s\n" "$out" | grep -qx "verify"'

built_completions="$(dirname "$(dirname "$oci")")/share/completions"
check "completion scripts are built for bash, zsh and fish" '[ -s "$built_completions/maelys-oci.bash" ] && [ -s "$built_completions/_maelys-oci" ] && [ -s "$built_completions/maelys-oci.fish" ]'

run missing "$oci" verify --store "$work/absent" --json --compact
check "absent store is a precondition failure" '[ "$code" = 1 ] && [ -z "$out" ] && printf "%s" "$err" | grep -q "\"code\":\"PRECONDITION_FAILED\""'

run list-absent "$oci" list --store "$work/absent" --json --compact
check "listing an absent store is an empty record set" '[ "$code" = 0 ] && [ -z "$err" ] && printf "%s" "$out" | grep -q "\"data\":{\"count\":0,\"records\":\[\]}"'

run list-jsonl "$oci" list --store "$work/absent" --format jsonl
check "jsonl records for an empty store print nothing" '[ "$code" = 0 ] && [ -z "$out" ] && [ -z "$err" ]'

run reference "$oci" remove not-a-reference --store "$work/absent" --json --compact
check "remove refuses a reference without a digest" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"VALIDATION_FAILED\""'

run source "$oci" inspect "$work/no-such-source" --json --compact
check "inspect reports a missing source as NOT_FOUND" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"NOT_FOUND\""'

mkdir -p "$work/not-a-layout"
run layout "$oci" inspect "$work/not-a-layout" --json --compact
check "inspect reports an invalid layout as PROTOCOL_FAILED" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "\"code\":\"PROTOCOL_FAILED\""'

run unpack "$oci" unpack-rootfs relative.tar "$work" --json --compact
check "unpack-rootfs operands are typed absolute paths" '[ "$code" = 1 ] && [ -z "$out" ] && printf "%s" "$err" | grep -q "\"code\":\"VALIDATION_FAILED\""'

# ---- dispatcher -------------------------------------------------------------------
commands="$work/commands"
mkdir -p "$commands"
digest=$(if command -v sha256sum >/dev/null 2>&1; then sha256sum "$oci"; else shasum -a 256 "$oci"; fi | awk '{print $1}')
cat >"$commands/oci.json" <<MANIFEST
{"schema":"maelys.cli-extension/v1","command":"oci","executable":"$oci","cliApi":1,"version":"0.1.0","summary":"Maelys OCI","sha256":"$digest"}
MANIFEST
export MAELYS_COMMANDS_PATH="$commands"

run d-list "$maelys" commands list --json --compact
check "dispatcher accepts the oci manifest with its digest" '[ "$code" = 0 ] && printf "%s" "$out" | grep -q "\"command\":\"oci\"" && printf "%s" "$out" | grep -q "\"digestVerified\":true"'

run d-version "$maelys" oci version --json --compact
check "maelys oci execs the terminal verbatim" '[ "$code" = 0 ] && printf "%s" "$out" | grep -q "\"product\":\"Maelys OCI\""'

run d-describe "$maelys" oci describe --summary --json --compact
check "maelys oci describe returns the product catalog" '[ "$code" = 0 ] && printf "%s" "$out" | grep -q "\"id\":\"import\""'

run d-exit "$maelys" oci verify --store "$work/absent"
check "dispatcher propagates the terminal exit code" '[ "$code" = 1 ] && printf "%s" "$err" | grep -q "PRECONDITION_FAILED"'

built_manifest="$(dirname "$(dirname "$oci")")/share/maelys/commands/oci.json"
check "built manifest is valid JSON binding the installed path" 'python3 -c "import json,sys; m=json.load(open(sys.argv[1])); assert m[\"schema\"]==\"maelys.cli-extension/v1\" and m[\"command\"]==\"oci\" and m[\"cliApi\"]==1 and m[\"executable\"].endswith(\"/bin/maelys-oci\") and len(m[\"sha256\"])==64" "$built_manifest"'

if [ "$failures" -ne 0 ]; then
    printf '%s CLI contract test(s) failed\n' "$failures" >&2
    exit 1
fi
printf 'cli-check: ok\n'
