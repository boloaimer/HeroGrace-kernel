#include <linux/err.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/printk.h>

#include "policy/allowlist.h"
#include "klog.h" // IWYU pragma: keep
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "manager/manager_observer.h"
#include "manager/throne_tracker.h"

bool ksu_module_mounted __read_mostly = false;
bool ksu_boot_completed __read_mostly = false;
extern void stop_input_hook();

extern void ksu_avc_spoof_late_init();

void on_post_fs_data(void)
{
	static bool done = false;
	if (done) {
		pr_info("on_post_fs_data already done\n");
		return;
	}
	done = true;
	pr_info("on_post_fs_data!\n");

	ksu_load_allow_list();
	ksu_observer_init();
	// sanity check, this may influence the performance
	stop_input_hook();
}


int nuke_ext4_sysfs(const char *mnt)
{
	pr_info("%s: feature not implemented!\n", __func__);
	return 0;
}

void on_module_mounted(void)
{
	pr_info("on_module_mounted!\n");
	ksu_module_mounted = true;
}

void on_boot_completed(void)
{
    ksu_boot_completed = true;
    pr_info("on_boot_completed!\n");
    track_throne(true);
    ksu_avc_spoof_late_init();
}
