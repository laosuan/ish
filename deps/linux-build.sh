#!/bin/bash -e
set -o pipefail
output="$1"
srctree="$2"
objtree="$3"
depfile="$4"

# https://stackoverflow.com/a/3572105/1455016
realpath() {
    [[ $1 = /* ]] && echo "$1" || echo "$PWD/${1#./}"
}

makeargs=()
makeargs+=("ARCH=ish")
if [[ -n "$HOSTCC" ]]; then
    makeargs+=("HOSTCC=$HOSTCC")
fi
if [[ -n "$CC" ]]; then
    makeargs+=("CC=$CC")
fi
makeargs+=("LLVM_IAS=1")

mkdir -p "$objtree"

defconfig="$srctree/arch/ish/configs/ish_defconfig"
for fragment in "$defconfig" $KCONFIG_FRAGMENTS; do
    if [[ "$fragment" -nt "$objtree/.config" ]]; then
        regen_config=1
    fi
done
if [[ -n "$regen_config" ]]; then
    export KCONFIG_CONFIG="$objtree/.config"
    "$srctree/scripts/kconfig/merge_config.sh" -m "$srctree/arch/ish/configs/ish_defconfig" $KCONFIG_FRAGMENTS
    unset KCONFIG_CONFIG
fi

case "$(uname)" in
    Darwin) cpus=$(sysctl -n hw.ncpu) ;;
    Linux) cpus=$(nproc) ;;
    *) cpus=1 ;;
esac

make -C "$srctree" O="$(realpath "$objtree")" "${makeargs[@]}" olddefconfig

(set -x; make -C "$objtree" -j "$cpus" "${makeargs[@]}" all compile_commands.json --debug=v) | "$srctree/../makefilter.py" "$depfile" "$output" "$objtree"
cp "$objtree/vmlinux" "$output"