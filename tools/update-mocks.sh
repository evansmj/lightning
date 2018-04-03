#!/bin/bash
# Script to rewrite the autogenerated mocks in a unit test between
# /* AUTOGENERATED MOCKS START */ and /* AUTOGENERATED MOCKS END */
# based on link failures.

set -e
FILE="$1"

BASE=/tmp/mocktmp.$$.$(echo "$@" | tr / _)
trap 'mv $BASE.old $FILE; rm -f $BASE.*' EXIT

START=$(fgrep -n '/* AUTOGENERATED MOCKS START */' "$FILE" | cut -d: -f1)
END=$(fgrep -n '/* AUTOGENERATED MOCKS END */' "$FILE" | cut -d: -f1)

if [ -n "$START" ]; then
    mv "$FILE" "${BASE}.old"
    echo "${FILE}:"
    head -n "$START" "${BASE}.old" > "$FILE"
    tail -n +"$END" "${BASE}.old" >> "$FILE"
    # Try to make binary.
    if ! make "${FILE/%.c/}" 2> "${BASE}.err" >/dev/null; then
	tools/mockup.sh < "${BASE}.err" >> "${BASE}.stubs"
	# If there are no link errors, maybe compile fail for other reason?
	if ! fgrep -q 'Generated stub for' "${BASE}.stubs"; then
	    cat "${BASE}.err"
	    exit 1
	fi
	sed -n 's,.*Generated stub for \(.*\) .*,\t\1,p' < "${BASE}.stubs"
	head -n "$START" "${BASE}.old" > "$FILE"
	cat "${BASE}.stubs" >> "$FILE"
	tail -n +"$END" "${BASE}.old" >> "$FILE"
    else
	echo "...build succeeded without stubs"
    fi
fi

# All good.
rm -f "$BASE".*
trap "" EXIT
