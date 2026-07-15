# TODO
# [ ] Writes to the write pointer are accepted
# [ ] Writes away from the write pointer are rejected
# [ ] Writes that cross zone boundaries are rejected
# [ ] Writes to closed zones implicitly open them.
#     QUESTION: is there a limit to the number of open zones?
# [ ] Writes to finished zones are rejected
# [ ] Writes to conventional zones ignore write pointer
# [ ] Writes may cross conventional zone boundaries
# [ ] Write pointers persist across device unload/reload
# [ ] "gzoned destroy" destroys devices and clears metadata
# [ ] "gzoned stop" stops devices, but leaves metadata in place
# [ ] "gzoned stop -f" works on open devices
# [ ] "gzoned create -s" changes the zone size
#     QUESTION: Is the metadata area of fixed size, or can it accomodate an
#     arbitrary number of zones?
# [ ] "gzoned create -r" creates some conventional zones
#   [ ] single zone
#   [ ] range
#   [ ] comma-separated list of multiple ranges and single zones
# [ ] "gzoned create" fails if the underlying device is already zoned
# [ ] "zonectl -c rz" reports zones
# [ ] "zonectl -c rz -l" reports zones with a starting LBA
# [ ] "zonectl -c params" reports parameters
# [ ] "zonectl -c open" opens a zone
# [ ] "zonectl -c close" closes a zone
# [ ] "zonectl -c finish" finishes a zone
# [ ] "zonectl -c open/close/finish" fails if the zone isn't in the
#     prerequisite state.
# [ ] "zonectl -c rwp" resets write pointer
# [ ] "zonectl -c rwp" Does the Right Thing for empty or finished zones
. $(atf_get_srcdir)/conf.sh

atf_test_case create cleanup
create_head()
{
	atf_set "descr" "Basic gzoned device creation"
	atf_set "require.user" "root"
}
create_body()
{
	gzoned_test_setup

	atf_check truncate -s 1g backing_file
	attach_md md -t vnode -f backing_file
	atf_check gzoned create -s 256m -r 256m whatname ${md}
	# TODO: check for /dev/mdXXX.zoned
	# TODO: check the number of zones and their states
}
create_cleanup()
{
	gzoned_test_cleanup
}

atf_init_test_cases()
{
	atf_add_test_case create
}
