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
}
create_cleanup()
{
	gzoned_test_cleanup
}

atf_init_test_cases()
{
	atf_add_test_case create
}
