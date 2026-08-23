// SPDX-License-Identifier: GPL-2.0-only
/*
 * DGX Spark DKMS-managed EC fan override / rollback sysfs driver
 *
 * Loading the module only binds the verified FF-A partition and creates:
 *
 *   /sys/bus/arm_ffa/devices/arm-ffa-17/fan
 *
 * Writing "max" or "auto" issues one OEM1 command 17 request. Requests are
 * serialized, and the interface remains available for subsequent state changes.
 */

#include <linux/arm_ffa.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>

#define ESPI_OEM_GENERIC_EMI       17U

#define ESPI_NS_SHM_PA             0x933dd000ULL
#define ESPI_NS_SHM_SIZE           0x1000U
#define ESPI_NS_SHM_PROTOCOL_SIZE  32U

#define GENERIC_FRAME_LENGTH       5U
#define GENERIC_OUTPUT_OFFSET      0U
#define GENERIC_DATA_OFFSET        0x10U
#define GENERIC_INPUT_ACCEPTED     3U
#define GENERIC_OUTPUT_READY       4U

#define THERMAL_OUTER_COMMAND      0x07U
#define THERMAL_SET_HIGH_OVERRIDE  0x05U
#define TARGET_FULL_RPM            13500U
#define TARGET_AUTOMATIC           0xffffU

#define REPLY_TIMEOUT_MS           5000U
#define REPLY_POLL_MS              10U
#define ESPI_TIMEOUT_FLOOR_US      40000LL

enum nvfancontrol_fan_state {
	NVFANCONTROL_READY = 0,
	NVFANCONTROL_MAX,
	NVFANCONTROL_AUTO,
	NVFANCONTROL_ERROR,
};

struct nvfancontrol_state {
	struct ffa_device *fdev;
	struct mutex request_lock;
	enum nvfancontrol_fan_state fan_state;
	int last_error;
};

static void restore_shared_page(struct device *dev, u8 *shm,
				const u8 snapshot[ESPI_NS_SHM_PROTOCOL_SIZE],
				int *result)
{
	memcpy(shm, snapshot, ESPI_NS_SHM_PROTOCOL_SIZE);
	mb();

	if (memcmp(shm, snapshot, ESPI_NS_SHM_PROTOCOL_SIZE)) {
		dev_crit(dev,
			 "SHARED-BUFFER RESTORE VERIFY FAILED at physical address %#llx\n",
			 ESPI_NS_SHM_PA);
		*result = -EUCLEAN;
		return;
	}

	dev_info(dev, "shared-buffer restore verified (first %u bytes)\n",
		 ESPI_NS_SHM_PROTOCOL_SIZE);
}

static int submit_fan_request(struct nvfancontrol_state *state, u16 target)
{
	struct ffa_device *fdev = state->fdev;
	struct ffa_send_direct_data2 msg = {};
	u8 snapshot[ESPI_NS_SHM_PROTOCOL_SIZE];
	u8 frame[ESPI_NS_SHM_PROTOCOL_SIZE] = {};
	u8 request[GENERIC_FRAME_LENGTH];
	u8 response[GENERIC_FRAME_LENGTH];
	u8 *payload = (u8 *)msg.data;
	u8 *shm;
	unsigned long pfn = PHYS_PFN(ESPI_NS_SHM_PA);
	unsigned long raw_response[4];
	u32 service_status;
	bool map_memory;
	bool reserved_page = false;
	unsigned int elapsed;
	ktime_t start;
	s64 elapsed_us;
	int result;
	int ret;

	map_memory = pfn_is_map_memory(pfn);
	if (map_memory)
		reserved_page = PageReserved(pfn_to_page(pfn));

	dev_info(&fdev->dev,
		 "manifest ns_shm0: physical=%#llx size=%#x map_memory=%u page_reserved=%u\n",
		 ESPI_NS_SHM_PA, ESPI_NS_SHM_SIZE,
		 (unsigned int)map_memory, (unsigned int)reserved_page);

	if (map_memory && !reserved_page) {
		dev_err(&fdev->dev,
			"refusing ns_shm0: PFN is Linux map memory but is not marked reserved\n");
		return -EPERM;
	}

	shm = memremap(ESPI_NS_SHM_PA, ESPI_NS_SHM_SIZE, MEMREMAP_WB);
	if (!shm) {
		dev_err(&fdev->dev, "memremap of manifest ns_shm0 failed\n");
		return -ENOMEM;
	}

	memcpy(snapshot, shm, sizeof(snapshot));
	dev_info(&fdev->dev, "shared pre-state: %*ph\n",
		 (int)sizeof(snapshot), snapshot);

	if (snapshot[GENERIC_INPUT_ACCEPTED] != 0 ||
	    snapshot[GENERIC_OUTPUT_READY] != 0) {
		dev_err(&fdev->dev,
			"refusing request: shared mailbox is not idle (accepted=%#04x ready=%#04x)\n",
			snapshot[GENERIC_INPUT_ACCEPTED],
			snapshot[GENERIC_OUTPUT_READY]);
		ret = -EBUSY;
		goto out_unmap;
	}

	request[0] = THERMAL_OUTER_COMMAND;
	request[1] = THERMAL_SET_HIGH_OVERRIDE;
	request[2] = 0;
	put_unaligned_le16(target, &request[3]);

	frame[0] = GENERIC_FRAME_LENGTH;
	frame[1] = GENERIC_FRAME_LENGTH;
	frame[2] = GENERIC_OUTPUT_OFFSET;
	memcpy(&frame[GENERIC_DATA_OFFSET], request, sizeof(request));
	memcpy(shm, frame, sizeof(frame));
	mb();

	if (target == TARGET_FULL_RPM)
		dev_warn(&fdev->dev,
			 "starting MAX request: fixed EC frame %*ph\n",
			 (int)sizeof(request), request);
	else
		dev_warn(&fdev->dev,
			 "starting AUTO request: fixed EC frame %*ph\n",
			 (int)sizeof(request), request);

	payload[0] = ESPI_OEM_GENERIC_EMI;
	start = ktime_get();
	ret = fdev->ops->msg_ops->sync_send_receive2(fdev, &msg);
	elapsed_us = ktime_us_delta(ktime_get(), start);
	if (ret) {
		dev_crit(&fdev->dev,
			 "FF-A TRANSPORT FAILURE: ret=%d elapsed=%lld us; EC override state is unknown\n",
			 ret, elapsed_us);
		restore_shared_page(&fdev->dev, shm, snapshot, &ret);
		goto out_unmap;
	}

	raw_response[0] = msg.data[0];
	raw_response[1] = msg.data[1];
	raw_response[2] = msg.data[2];
	raw_response[3] = msg.data[3];
	service_status = get_unaligned_le32((u8 *)msg.data);
	dev_info(&fdev->dev,
		 "command-17 response: status=%u elapsed=%lld us raw=%#018lx %#018lx %#018lx %#018lx\n",
		 service_status, elapsed_us, raw_response[0], raw_response[1],
		 raw_response[2], raw_response[3]);

	if (service_status != 0) {
		if (service_status == 5 && elapsed_us >= ESPI_TIMEOUT_FLOOR_US)
			dev_crit(&fdev->dev,
				 "eSPI TIMEOUT: internal mailbox operation timed out; EC override state is unknown\n");
		else
			dev_crit(&fdev->dev,
				 "SERVICE REJECTED: status=%u; EC override state is unknown\n",
				 service_status);

		ret = service_status == 10 ? -EBUSY : -EIO;
		restore_shared_page(&fdev->dev, shm, snapshot, &ret);
		goto out_unmap;
	}

	for (elapsed = 0; elapsed < REPLY_TIMEOUT_MS;
	     elapsed += REPLY_POLL_MS) {
		if (READ_ONCE(shm[GENERIC_OUTPUT_READY]) == 1)
			break;
		msleep(REPLY_POLL_MS);
	}

	if (READ_ONCE(shm[GENERIC_OUTPUT_READY]) != 1) {
		dev_crit(&fdev->dev,
			 "OUTPUT TIMEOUT after %u ms: accepted=%#04x ready=%#04x; EC override state is unknown; shared frame is left intact and no retry is allowed\n",
			 REPLY_TIMEOUT_MS,
			 READ_ONCE(shm[GENERIC_INPUT_ACCEPTED]),
			 READ_ONCE(shm[GENERIC_OUTPUT_READY]));
		ret = -ETIMEDOUT;
		goto out_unmap;
	}

	mb();
	memcpy(response, &shm[GENERIC_DATA_OFFSET], sizeof(response));
	dev_info(&fdev->dev,
		 "EC reply after <=%u ms: accepted=%#04x ready=%#04x response=%*ph\n",
		 elapsed, READ_ONCE(shm[GENERIC_INPUT_ACCEPTED]),
		 READ_ONCE(shm[GENERIC_OUTPUT_READY]),
		 (int)sizeof(response), response);

	if (memcmp(response, request, sizeof(response)) || response[2] != 0) {
		dev_crit(&fdev->dev,
			 "RESPONSE MISMATCH: expected=%*ph observed=%*ph; EC override state is unknown\n",
			 (int)sizeof(request), request,
			 (int)sizeof(response), response);
		result = -EPROTO;
		restore_shared_page(&fdev->dev, shm, snapshot, &result);
		ret = result;
		goto out_unmap;
	}

	if (target == TARGET_FULL_RPM)
		dev_warn(&fdev->dev,
			 "MAX ACKNOWLEDGED: EC accepted high override=13500 RPM; fan0 and fan1 policy outputs saturate at 100%%\n");
	else
		dev_warn(&fdev->dev,
			 "AUTO ACKNOWLEDGED: EC accepted high override=0xffff; automatic thermal curve is restored\n");

	result = 0;
	restore_shared_page(&fdev->dev, shm, snapshot, &result);
	ret = result;

out_unmap:
	memunmap(shm);
	return ret;
}

static ssize_t fan_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct nvfancontrol_state *state = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&state->request_lock);
	switch (state->fan_state) {
	case NVFANCONTROL_MAX:
		length = sysfs_emit(buf, "max\n");
		break;
	case NVFANCONTROL_AUTO:
		length = sysfs_emit(buf, "auto\n");
		break;
	case NVFANCONTROL_ERROR:
		length = sysfs_emit(buf, "error %d\n", state->last_error);
		break;
	case NVFANCONTROL_READY:
	default:
		length = sysfs_emit(buf, "ready\n");
		break;
	}
	mutex_unlock(&state->request_lock);

	return length;
}

static ssize_t fan_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct nvfancontrol_state *state = dev_get_drvdata(dev);
	enum nvfancontrol_fan_state requested_state;
	u16 target;
	int ret;

	if (sysfs_streq(buf, "max")) {
		target = TARGET_FULL_RPM;
		requested_state = NVFANCONTROL_MAX;
	} else if (sysfs_streq(buf, "auto")) {
		target = TARGET_AUTOMATIC;
		requested_state = NVFANCONTROL_AUTO;
	} else {
		dev_err(dev, "fan accepts only 'max' or 'auto'\n");
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&state->request_lock);
	if (ret)
		return ret;

	ret = submit_fan_request(state, target);
	if (ret) {
		state->fan_state = NVFANCONTROL_ERROR;
		state->last_error = ret;
	} else {
		state->fan_state = requested_state;
		state->last_error = 0;
	}

	mutex_unlock(&state->request_lock);
	return ret ? ret : count;
}

static DEVICE_ATTR_RW(fan);

static int fan_override_probe(struct ffa_device *fdev)
{
	struct nvfancontrol_state *state;
	int ret;

	if (!fdev->ops || !fdev->ops->msg_ops ||
	    !fdev->ops->msg_ops->sync_send_receive2) {
		dev_err(&fdev->dev, "FF-A Direct Request 2 is unavailable\n");
		return -EOPNOTSUPP;
	}

	dev_info(&fdev->dev,
		 "matched: partition=%#x uuid=%pUb properties=%#x mode_32bit=%u boot-age=%llu s\n",
		 fdev->id, &fdev->uuid, fdev->properties,
		 (unsigned int)fdev->mode_32bit,
		 (unsigned long long)ktime_get_boottime_seconds());

	if (fdev->mode_32bit) {
		dev_err(&fdev->dev,
			"refusing Direct Request 2 for a 32-bit-mode device\n");
		return -EOPNOTSUPP;
	}

	state = devm_kzalloc(&fdev->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->fdev = fdev;
	state->fan_state = NVFANCONTROL_READY;
	mutex_init(&state->request_lock);
	dev_set_drvdata(&fdev->dev, state);

	ret = device_create_file(&fdev->dev, &dev_attr_fan);
	if (ret) {
		dev_err(&fdev->dev, "failed to create sysfs fan attribute: %d\n",
			ret);
		return ret;
	}

	dev_info(&fdev->dev,
		 "sysfs control ready: %s/fan accepts 'max' or 'auto'; module load issued no EC request\n",
		 dev_name(&fdev->dev));
	return 0;
}

static void fan_override_remove(struct ffa_device *fdev)
{
	device_remove_file(&fdev->dev, &dev_attr_fan);
	dev_warn(&fdev->dev,
		 "module removed; unloading does NOT change the EC override value\n");
}

static const struct ffa_device_id fan_override_ids[] = {
	{
		.uuid = UUID_INIT(0x884a63a0, 0x3285, 0x4120,
				  0x83, 0xaa, 0xee, 0xc0,
				  0x08, 0xa0, 0xa5, 0x46),
	},
	{},
};

static struct ffa_driver fan_override_driver = {
	.name = "nvfancontrol",
	.probe = fan_override_probe,
	.remove = fan_override_remove,
	.id_table = fan_override_ids,
};

module_ffa_driver(fan_override_driver);

MODULE_DESCRIPTION("DGX Spark persistent EC fan sysfs controller");
MODULE_AUTHOR("841973620");
MODULE_LICENSE("GPL");
