class="zoned"

gzoned_test_setup()
{
	geom_atf_test_setup
}

gzoned_test_cleanup()
{
	if [ -f "$TEST_MDS_FILE" ]; then
		while read md; do
			[ -c /dev/${md}.zoned ] && \
				gzoned destroy $md.zoned 2>/dev/null
			mdconfig -d -u $md 2>/dev/null
		done < $TEST_MDS_FILE
	fi
	true
}

. `dirname $0`/../geom_subr.sh

