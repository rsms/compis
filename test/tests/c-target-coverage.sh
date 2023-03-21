cd "$TEST_DIR"/tests

# start by recording all existing target test scripts as "covered"
for tvar in c-target-*.sh; do
  tvar=$(basename $tvar .sh)
  tvar=${tvar:9} # len("c-target-")=9
  tvar=${tvar//./_DOT_}
  tvar=${tvar//-/__}
  declare COVERED_${tvar}=1
done

# iterate over all supported targets, adding targets without coverage to MISSING
MISSING=()
for target in $(co targets); do
  #
  # Should a target not have test coverage?
  # Add an exception here by testing $target and including a brief reason.
  #
  # Example:
  #   # No coverage for somesys since <explanation here>
  #   [[ $target == *-somesys ]] && continue
  #
  tvar=$target
  tvar=${tvar//./_DOT_}
  tvar=${tvar//-/__}
  tvar=COVERED_${tvar}
  if [ -z "${!tvar:-}" ]; then
    MISSING+=( $target )
  fi
done

# exit now if all targets are covered
[ ${#MISSING[@]} -gt 0 ] || { echo "all targets covered"; exit 0; }

# report
echo "warning: missing test coverage for ${#MISSING[@]} targets" >&2

# short message if not in verbose mode
$VERBOSE || { echo "Run with -v for more details" >&2; exit 0; }

# list all missing targets and some help
echo "Here's how you can quickly add the missing test(s):"
for target in ${MISSING[@]}; do
  source=c-target-aarch64-linux.sh
  [[ "$target" == *-none ]] && source=c-target-aarch64-none.sh
  echo "ln -s $source test/tests/c-target-${target}.sh"
done

cat << END

If you need a custom test, create a new test like this:
  sed -E 's/--target=([^ ]+)/--target=${MISSING[0]}/' \\
    test/tests/c-target-aarch64-linux.sh > \\
    test/tests/c-target-${MISSING[0]}.sh
  \$EDITOR test/tests/c-target-${MISSING[0]}.sh
END
