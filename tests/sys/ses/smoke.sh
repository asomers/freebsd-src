# Basic smoke test of the ioctl interface.  Compares the ioctls' output to that
# of sg_ses, an independent SES implementation.

SRCDIR=$( atf_get_srcdir )

for_each_ses_dev()
{
	cb=$1

	for devname in /dev/ses*; do
		if [ "$devname" = "/dev/ses*" ]; then
			ATF_SKIP "No ses devices found"
		fi
		$cb $devname
	done
}

do_getelmdesc()
{
	dev=$1

	nelm=`${SRCDIR}/helper $dev ENCIOC_GETNELM`
	for i in `seq 1 "${nelm}"`; do
		actual=`${SRCDIR}/helper $dev ENCIOC_GETELMDESC $i`
	done
	expected=`sg_ses -p1 $dev | \
		awk '/enclosure logical identifier/ {print $NF}'`
	atf_check_equal "$expected" "$actual"
}

atf_test_case getelmdesc
getelmdesc_head()
{
	atf_set "descr" "Compare ENCIOC_GETELMDESC to sg3_utils' output"
	atf_set "require.user" "root"
	atf_require_prog "sg_ses"
}
getelmdesc_body()
{
	for_each_ses_dev do_getelmdesc
}

do_getencid()
{
	dev=$1

	expected=`sg_ses -p1 $dev | \
		awk '/enclosure logical identifier/ {print $NF}'`
	actual=`${SRCDIR}/helper $dev ENCIOC_GETENCID`
	atf_check_equal "$expected" "$actual"
}

atf_test_case getencid
getencid_head()
{
	atf_set "descr" "Compare ENCIOC_GETDEVID to sg3_utils' output"
	atf_set "require.user" "root"
	atf_require_prog "sg_ses"
}
getencid_body()
{
	for_each_ses_dev do_getencid
}

do_getencname()
{
	dev=$1

	expected=`sg_inq -o $dev | awk '
		/Vendor identification/ {vi=$NF}
		/Product identification/ {pi=$NF}
		/Product revision level/ {prl=$NF}
		END {print(vi " " pi " " prl)}
	'`
	actual=`${SRCDIR}/helper $dev ENCIOC_GETENCNAME`
	atf_check_equal "$expected" "$actual"
}

atf_test_case getencname
getencname_head()
{
	atf_set "descr" "Compare ENCIOC_GETDEVNAME to sg3_utils' output"
	atf_set "require.user" "root"
	atf_require_prog "sg_inq"
}
getencname_body()
{
	for_each_ses_dev do_getencname
}

do_getencstat()
{
	dev=$1

	expected=`sg_ses -p2 $dev | \
		awk '/INVOP=/ {print}'`
	actual=`${SRCDIR}/helper $dev ENCIOC_GETENCSTAT`
	atf_check_equal "$expected" "$actual"
}

atf_test_case getencstat
getencstat_head()
{
	atf_set "descr" "Compare ENCIOC_GETENCSTAT to sg3_utils' output"
	atf_set "require.user" "root"
	atf_require_prog "sg_ses"
}
getencstat_body()
{
	for_each_ses_dev do_getencstat
}

do_getnelm()
{
	dev=$1

	expected=`sg_ses -p1 $dev | awk '
		/number of possible elements:/ {nelm = nelm + 1 + $NF}
		END {print(nelm)}
	'`
	actual=`${SRCDIR}/helper $dev ENCIOC_GETNELM`
	atf_check_equal "$expected" "$actual"
}

atf_test_case getnelm
getnelm_head()
{
	atf_set "descr" "Compare ENCIOC_GETNELM to sg3_utils' output"
	atf_set "require.user" "root"
	atf_require_prog "sg_ses"
}
getnelm_body()
{
	for_each_ses_dev do_getnelm
}

atf_init_test_cases()
{
	atf_add_test_case getencid
	atf_add_test_case getencname
	atf_add_test_case getencstat
	atf_add_test_case getnelm
	# ENCIOC_INIT is untested because SES doesn't need it and I don't have
	# any SAF-TE devices.
	# ENCIOC_SETSTRING is untested because it's seriously unsafe!  It's
	# normally used for stuff like firmware updates.
}
