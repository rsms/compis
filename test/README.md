# Tests

This directory contains the test suite,
a set of scripts in `tests` which are run by `test.sh`

To run all tests:

    ./test.sh

For help on `test.sh` usage:

    ./test.sh --help


## Test scripts

A test script is a simple Bash script. Here's an example:

```bash
co cc -c hello.c -o hello.o
co cc hello.o -o hello.exe
./hello.exe
```

Test scripts must be put in the `tests` directory.
You can put them in subdirectories if you need any test-specific data to go with it.

Every test run executes a test script in its own newly-created directory,
with its working directory (i.e `cwd`, `$PWD`) set to that directory.
Its working directory is prepopulated with the contents of the template `data` directory.
This allows test scripts to not have to worry about effects of other tests and reduces
the possiblity of bugs arising from the test system itself.

All bash function in `{SRCROOT}/etc/lib.sh` and `libtest.sh` are available
in test scripts.
Additionally, the compis build being tested is in `PATH`.
I.e. `co`, `cc`, `ar`, etc are all the compis executable being tested.

Test scripts are run with the following implicit header:

```bash
#!/usr/bin/env bash
set -euo pipefail
source "\$PROJECT/test/libtest.sh"
# {actual test script pasted here}
```

Effectively, this is what `test.sh` does for each test script:

```bash
mkdir out/test/mytestscriptname
cd out/test/mytestscriptname
cat HEADER > mytestscriptname.sh
cat mytestscript.sh >> mytestscriptname.sh
cp -r TEST_DIR/data/* ./
bash mytestscriptname.sh > stdout.log 2> stderr.log
```
