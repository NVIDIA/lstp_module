#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Export the upstream copy of the LSTP driver into a kernel tree.
#
# Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
#
# Usage: export-upstream.sh [--bmc] <dest>
#
# Copies the files that go upstream to the same paths below <dest>, a kernel
# tree or an empty directory, and resolves every LSTP_COMPAT_* conditional to
# its upstream branch with unifdef. Device tree bindings go to
# Documentation/devicetree/bindings/, and the pages' references to them
# follow. Out-of-tree only, and not copied: the compat header, Kbuild,
# Makefile, Kconfig, the IPMI POST-code header and the MMIO driver. In a
# binding, the lines from a "# out-of-tree only: begin" line to the next
# "# out-of-tree only: end" line document what only this package reads, and
# are dropped.
#
# When <dest> is a kernel tree (it has a MAINTAINERS file), also applies
# scripts/upstream-hookup.patch: the in-tree Kconfig and Makefile entries,
# the MAINTAINERS entry and the toctree entries of the two documentation
# pages. The patch is made against the kernel release the export targets;
# refresh it when it no longer applies, which fails the export.
#
# --bmc makes the copy for a BMC kernel tree instead: it keeps the
# auto_bind_spidev parameter, the IPMI POST-code node and the deployed I2C
# "retries" property, resolving their LSTP_COMPAT_* conditionals to this
# package's branch, keeps the bindings' out-of-tree-only lines, and copies
# the POST-code header. In a kernel tree it then also applies
# scripts/upstream-hookup-bmc.patch, which adds their Kconfig options with
# the defaults of the out-of-tree build.
#
# Fails, before copying anything, if the sources break the rules that make
# the copy safe to generate:
#   - only compat/lstp-compat.h defines LSTP_COMPAT_* symbols;
#   - a conditional tests one LSTP_COMPAT_* symbol, with #ifdef or #ifndef,
#     and nothing else;
#   - no source names a symbol the compat header does not define;
# and, after unifdef, if the copy still names an LSTP_COMPAT_* symbol or a
# kernel version check.
#
# Needs unifdef 2.12: UNIFDEF names the binary, by default unifdef in PATH.

set -euo pipefail

UNIFDEF=${UNIFDEF:-unifdef}
SRC=$(cd "$(dirname "$0")/.." && pwd)
COMPAT=compat/lstp-compat.h
HOOKUP=scripts/upstream-hookup.patch
BMC_HOOKUP=scripts/upstream-hookup-bmc.patch
# Sources that only the out-of-tree package builds.
OUT_OF_TREE_ONLY=(drivers/char/ipmi/lstp-ipmi-postcodes.h drivers/misc/lstp-mmio.c)
# What --bmc keeps: the features, and the sources only they use.
BMC_FEATURES=(LSTP_COMPAT_AUTO_BIND_SPIDEV LSTP_COMPAT_IPMI_POSTCODES
	LSTP_COMPAT_I2C_UNPREFIXED_RETRIES)
BMC_SOURCES=(drivers/char/ipmi/lstp-ipmi-postcodes.h)
OOT_BEGIN='# out-of-tree only: begin'
OOT_END='# out-of-tree only: end'

die() {
	echo "export-upstream: $*" >&2
	exit 1
}

bmc=0
if [ "${1:-}" = --bmc ]; then
	bmc=1
	shift
fi
[ $# -eq 1 ] || die "usage: $0 [--bmc] <dest>"
mkdir -p "$1"
DEST=$(cd "$1" && pwd)

ver=$("$UNIFDEF" -V 2>&1 | sed -n 's/^Version: unifdef-//p' | tr -d ' ') ||
	die "cannot run $UNIFDEF"
[ "$ver" = 2.12 ] || die "need unifdef 2.12, found ${ver:-none} at $UNIFDEF"

cd "$SRC"

mapfile -t syms < <(sed -nE 's/^#define (LSTP_COMPAT_[A-Z0-9_]+)( .*)?$/\1/p' "$COMPAT" | sort -u)
[ ${#syms[@]} -gt 0 ] || die "$COMPAT defines no LSTP_COMPAT_* symbol"

keep=()
skip=("${OUT_OF_TREE_ONLY[@]}")
if [ $bmc -eq 1 ]; then
	for s in "${BMC_FEATURES[@]}"; do
		printf '%s\n' "${syms[@]}" | grep -qx "$s" || die "$COMPAT does not define $s"
	done
	keep=("${BMC_FEATURES[@]}")
	mapfile -t skip < <(printf '%s\n' "${OUT_OF_TREE_ONLY[@]}" |
		grep -vxF -f <(printf '%s\n' "${BMC_SOURCES[@]}"))
fi

files=()
while IFS= read -r f; do
	for s in "${skip[@]}"; do
		[ "$f" = "$s" ] && continue 2
	done
	files+=("$f")
done < <(find drivers include Documentation devicetree -type f \
	\( -name '*.[ch]' -o -name '*.rst' -o -name '*.yaml' -o -path 'Documentation/ABI/*' \) |
	LC_ALL=C sort)
mapfile -t csrc < <(printf '%s\n' "${files[@]}" | grep -E '\.[ch]$')
[ ${#csrc[@]} -gt 0 ] || die "no sources found below $SRC"

bad=0
if grep -nE '^\s*#\s*define\s+LSTP_COMPAT_' "${csrc[@]}" "${OUT_OF_TREE_ONLY[@]}"; then
	echo "export-upstream: LSTP_COMPAT_* defined outside $COMPAT" >&2
	bad=1
fi
if grep -nE '^\s*#\s*(if|elif).*\bLSTP_COMPAT_' "${csrc[@]}" |
	grep -vE ':\s*#\s*if(n)?def\s+LSTP_COMPAT_[A-Z0-9_]+\s*(/\*.*\*/)?\s*$'; then
	echo "export-upstream: an LSTP_COMPAT_* conditional must be" \
		"#ifdef or #ifndef on one symbol" >&2
	bad=1
fi
while IFS= read -r s; do
	if ! printf '%s\n' "${syms[@]}" | grep -qx "$s"; then
		echo "export-upstream: $s is not defined in $COMPAT" >&2
		bad=1
	fi
done < <(grep -ohE '\bLSTP_COMPAT_[A-Z0-9_]+' "${csrc[@]}" | sort -u)
[ $bad -eq 0 ] || exit 1

out=()
for f in "${files[@]}"; do
	case $f in
	devicetree/*) t=Documentation/$f ;;
	*) t=$f ;;
	esac
	mkdir -p "$DEST/$(dirname "$t")"
	cp "$f" "$DEST/$t"
	out+=("$t")
done

cd "$DEST"
mapfile -t yout < <(printf '%s\n' "${out[@]}" | grep -E '\.yaml$')
for f in "${yout[@]}"; do
	[ $bmc -eq 0 ] || break
	# A blank line on each side of a dropped block leaves one, not two.
	awk -v b="$OOT_BEGIN" -v e="$OOT_END" '
		{ t = $0; sub(/^[ \t]+/, "", t) }
		t == b { bad = bad || in_oot; in_oot = 1; next }
		t == e { bad = bad || !in_oot; in_oot = 0; cut = 1; next }
		in_oot { next }
		cut && $0 == "" && last == "" { cut = 0; next }
		{ print; last = $0; cut = 0 }
		END { exit bad || in_oot }' "$f" >"$f.tmp" ||
		die "$f: unbalanced \"$OOT_BEGIN\" and \"$OOT_END\" lines"
	mv "$f.tmp" "$f"
done

mapfile -t rout < <(printf '%s\n' "${out[@]}" | grep -E '\.rst$')
b=devicetree/bindings/
[ ${#rout[@]} -eq 0 ] || sed -i -E "s#(^|[^/[:alnum:]])$b#\\1Documentation/$b#g" "${rout[@]}"

mapfile -t cout < <(printf '%s\n' "${out[@]}" | grep -E '\.[ch]$')
uargs=()
for s in "${syms[@]}"; do
	if printf '%s\n' "${keep[@]}" | grep -qx "$s"; then
		uargs+=(-D"$s")
	else
		uargs+=(-U"$s")
	fi
done
# -B: a removed block leaves one blank line, not the two around it.
"$UNIFDEF" -x 2 -m -B "${uargs[@]}" "${cout[@]}" || die "unifdef failed"

if [ -f MAINTAINERS ]; then
	hookups=("$HOOKUP")
	[ $bmc -eq 0 ] || hookups+=("$BMC_HOOKUP")
	for h in "${hookups[@]}"; do
		patch -p1 -N -s --no-backup-if-mismatch -i "$SRC/$h" ||
			die "$h does not apply to $DEST"
		mapfile -t hook < <(sed -n 's#^+++ b/##p' "$SRC/$h")
		for f in "${hook[@]}"; do
			printf '%s\n' "${out[@]}" | grep -qxF "$f" || out+=("$f")
		done
	done
fi

bad=0
if grep -nE '\bLSTP_COMPAT_' "${out[@]}"; then
	echo "export-upstream: LSTP_COMPAT_ left in the export" >&2
	bad=1
fi
if [ $bmc -eq 0 ] && [ ${#yout[@]} -gt 0 ] && grep -nF "# out-of-tree only:" "${yout[@]}"; then
	echo "export-upstream: out-of-tree-only binding lines left in the export" >&2
	bad=1
fi
if grep -nE 'LINUX_VERSION_CODE|KERNEL_VERSION\(|<linux/version\.h>' "${cout[@]}"; then
	echo "export-upstream: kernel version check left in the export" >&2
	bad=1
fi
[ $bad -eq 0 ] || exit 1

printf '%s\n' "${out[@]}"
