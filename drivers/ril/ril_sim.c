#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/switch.h>

#include <mach/board-cardhu-misc.h>

#include "baseband-xmm-power.h"
#include "ril.h"
#include "ril_sim.h"

#define NAME_SIM_PLUG "ril_sim_plug"

//**** external symbols

extern void baseband_xmm_ap_resume_work(void);


//**** constants

#define _ATTR_MODE S_IRUSR | S_IWUSR | S_IRGRP

#define SIM_PLUG_STATE_ABSENT  1
#define SIM_PLUG_STATE_PLUGGED 0

#define SYSFS_VAL_PLUG_STATE_FREEZED 1
#define SYSFS_VAL_PLUG_STATE_ACTIVATED 0


//**** local variable declaration

static struct device *dev;

static struct workqueue_struct *workqueue;

static struct delayed_work hotplug_work_task;
static struct switch_dev sim_sdev;
static int sim_plug_state;

static struct work_struct modem_reset_start_task;
static struct work_struct modem_reset_finish_task;
static bool is_sim_plug_state_freezed = false;


//**** callbacks for switch device

static ssize_t print_sim_plug_name(struct switch_dev *sdev, char *buf)
{
	return sprintf(buf, "%s\n", NAME_SIM_PLUG);
}

static ssize_t print_sim_plug_state(struct switch_dev *sdev, char *buf)
{
	return sprintf(buf, "%d\n", sim_plug_state);
}


//**** IRQ event handler

static int get_sim_plug_state_from_pin()
{
	int ping_state = gpio_get_value(SIM_CARD_DET);
	if (0 == ping_state) {
		return SIM_PLUG_STATE_ABSENT;
	} else {
		return SIM_PLUG_STATE_PLUGGED;
	}
}

static void hotplug_work_handle(void)
{
	// workaround: if reseting modem, some noisy IRQs would be raised.
	// Ignore plug state here, and we'll correct the state later when
	// finishing reseting.
	if (is_sim_plug_state_freezed) {
		RIL_INFO("sim state changed when state freezed, ignore.\n");
		return;
	}

	// refresh sysfs with current GPIO state
	sim_plug_state = get_sim_plug_state_from_pin();
	RIL_INFO("sim state = %d\n", sim_plug_state);
	switch_set_state(&sim_sdev, sim_plug_state);

	// wake up modem to read card state
	if (TEGRA3_PROJECT_TF300TG == tegra3_get_project_id()) {
		baseband_xmm_ap_resume_work();
	} else {
		// do nothing
	}
}

irqreturn_t sim_interrupt_handle(int irq, void *dev_id)
{
	const unsigned long WORK_DELAY = HZ / 10;
	queue_delayed_work(workqueue, &hotplug_work_task, WORK_DELAY);
	return IRQ_HANDLED;
}


//**** modem reset event handler

static void freeze_sim_plug_state_work_handle(void)
{
	RIL_INFO("freeze changing plug state\n");
	is_sim_plug_state_freezed = true;
}

static void release_sim_plug_state_work_handle(void)
{
	RIL_INFO("active changing plug state\n");
	is_sim_plug_state_freezed = false;
	hotplug_work_handle();
}

//**** sysfs callback functions

static ssize_t show_hotplug_detect_state(
  struct device *class,
  struct device_attribute *attr,
  char *buf)
{
	int result;
	if (true == is_sim_plug_state_freezed) {
		result = SYSFS_VAL_PLUG_STATE_FREEZED;
	} else {
		result = SYSFS_VAL_PLUG_STATE_ACTIVATED;
	}
	return sprintf(buf, "%d\n", result);
}

static ssize_t store_hotplug_detect_state(
  struct device *class,
  struct device_attribute *attr,
  const char *buf,
  size_t count)
{
	int enableVal = -1;

	if ( 1 != sscanf(buf, "%u", &enableVal)) {
		return -EINVAL;
	}

	switch (enableVal) {
	case SYSFS_VAL_PLUG_STATE_FREEZED:
		queue_work(workqueue, &modem_reset_start_task);
		break;
	case SYSFS_VAL_PLUG_STATE_ACTIVATED:
		queue_work(workqueue, &modem_reset_finish_task);
		break;
	default:
		RIL_ERR("%s: unknown value %d\n", __func__, enableVal);
		break;
	}

	return strnlen(buf, count);
}


//**** sysfs list

static struct device_attribute device_attr_TF300TG[] = {
	__ATTR(stop_hotplug_detect, _ATTR_MODE, show_hotplug_detect_state, store_hotplug_detect_state),
	__ATTR_NULL,
};

static struct device_attribute device_attr_TF300TL[] = {
	__ATTR_NULL,
};


//**** initialize and finalize

static void null_work_handle(void)
{
	panic("default_work_handle should NOT be invoked (%s#%d)", __FILE__, __LINE__);
}

int sim_hot_plug_init(struct device *target_device, struct workqueue_struct *queue)
{
	int rc = 0, i = 0;
	int sim_irq = gpio_to_irq(SIM_CARD_DET);
	sim_plug_state = get_sim_plug_state_from_pin();
	unsigned int project = tegra3_get_project_id();
	struct device_attribute *device_attr_list;
	int device_attr_list_len;

	dev = target_device;

	RIL_INFO(
	  "GPIO = %d , irq = %d, state = %d\n",
	  SIM_CARD_DET,
	  sim_irq,
	  sim_plug_state);

	// init work queue and delayed work
	workqueue = queue;
	INIT_DELAYED_WORK(&hotplug_work_task, hotplug_work_handle);

	if (TEGRA3_PROJECT_TF300TG == project) {
		INIT_WORK(&modem_reset_start_task, freeze_sim_plug_state_work_handle);
		INIT_WORK(&modem_reset_finish_task, release_sim_plug_state_work_handle);
		device_attr_list = device_attr_TF300TG;
		device_attr_list_len = ARRAY_SIZE(device_attr_TF300TG);
	} else if (TEGRA3_PROJECT_TF300TL == project) {
		INIT_WORK(&modem_reset_start_task, null_work_handle);
		INIT_WORK(&modem_reset_finish_task, null_work_handle);
		device_attr_list = device_attr_TF300TL;
		device_attr_list_len = ARRAY_SIZE(device_attr_TF300TL);
	} else {
		return -1;
	}

	// create sysfses
	for (i = 0; i < (device_attr_list_len - 1); i++) {
		rc = device_create_file(dev, &device_attr_list[i]);
		if (rc < 0) {
			RIL_ERR("%s: create file of [%d] failed, err = %d\n", __func__, i, rc);
			goto create_sysfs_failed;
		}
	}

	// register switch class
	rc = 0;
	sim_sdev.name = NAME_SIM_PLUG;
	sim_sdev.print_name = print_sim_plug_name;
	sim_sdev.print_state = print_sim_plug_state;
	rc = switch_dev_register(&sim_sdev);
	// Because switch_dev_register will initiate sdev.state to 0,
	// sdev.state will be initiated after switch_dev_register.
	sim_sdev.state = sim_plug_state;

	if (rc < 0) {
		RIL_ERR("Could not register switch device, rc = %d\n", rc);
		return -1;
	}

	RIL_INFO("request switch class successfully\n");
	return 0;

create_sysfs_failed:
	while (i >= 0) {
		device_remove_file(dev, &device_attr_list[i]);
		--i;
	}
	return rc;
}

void sim_hot_plug_exit(void)
{
	int i;

	struct device_attribute *device_attr_list;
	int device_attr_list_len;

	// destroy switch devices
	switch_dev_unregister(&sim_sdev);

	// destroy sysfses
	unsigned int project = tegra3_get_project_id();
	if (TEGRA3_PROJECT_TF300TG == project) {
		device_attr_list = device_attr_TF300TG;
		device_attr_list_len = ARRAY_SIZE(device_attr_TF300TG);
	} else {
		device_attr_list = device_attr_TF300TL;
		device_attr_list_len = ARRAY_SIZE(device_attr_TF300TL);
	}

	for (i = 0; i < (device_attr_list_len - 1); i++) {
		device_remove_file(dev, &device_attr_list[i]);
	}
}

