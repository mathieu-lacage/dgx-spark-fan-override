// SPDX-License-Identifier: GPL-2.0-only
/*
 * DGX Spark EC SRAM peek tool (FF-A eSPI-service Memory32 read)
 *
 * The MSI EdgeXpert / DGX Spark system firmware exposes an FF-A "eSPI
 * Service" secure partition (UUID 884a63a0-3285-4120-83aa-eec008a0a546,
 * bound in Linux as /sys/bus/arm_ffa/devices/arm-ffa-17). Its OEM1
 * command dispatcher (selected by the partition UUID the FF-A layer puts
 * in x2:x3) implements, among others:
 *
 *   cmd 0x0A  single Memory32 read   payload: [0]=0x0A [1..4]=addr LE32
 *   cmd 0x0C  var-length Mem32 read  payload: [0]=0x0C [1..4]=addr LE32
 *                                             [5..8]=count LE32
 *
 * Unlike the EMI-mailbox command (0x11) that nvfancontrol uses, the
 * Memory32 read does NOT touch the ns_shm shared page: the requested
 * bytes are returned little-endian directly in the Direct-Request-2
 * reply registers, starting at data[0] byte 0. On an eSPI bus error the
 * partition returns 0 rather than a status code, so a read that comes
 * back all-zero is ambiguous (either genuine zero memory or an
 * unmapped/eSPI-error address) -- verify the window first (see below).
 *
 * The eSPI Memory32 address is the eSPI bus address, NOT the EC's own
 * SRAM address. The EC SRAM page at EC 0x00119000 is mapped to eSPI
 * 0x06000000, so:
 *
 *   EC 0x00119190 (override-low slot)      -> eSPI 0x06000190
 *   EC 0x00119e70 (active thermal-table ptr) -> eSPI 0x06000e70
 *
 * This module creates three sysfs attributes on the FF-A device:
 *
 *   ec_mem32   (RW)  write "<hexaddr> [hexcount]" to set the eSPI address
 *                    (default count 4); reading performs the read live and
 *                    prints the bytes in memory order as hex.
 *   ec_profile (RO)  reads the 4-byte pointer at eSPI 0x06000e70 and
 *                    decodes which static thermal profile is active:
 *                    0x000d0fd4 => "A" (boot flag set), 0x000d0fe8 => "B".
 *   ec_verify  (RO)  reads the 2-byte override-low slot at eSPI 0x06000190;
 *                    compare against nvfancontrol's fan_override "low" to
 *                    confirm the EC-SRAM-to-eSPI window mapping before
 *                    trusting ec_profile.
 *
 * NOTE: only one FF-A driver can bind arm-ffa-17 at a time, so unload
 * nvfancontrol before loading this module (and vice versa). This module
 * only ever issues Memory32 *reads*; it never writes EC state.
 */

#include <linux/arm_ffa.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/hex.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>

#define ESPI_OEM_MEM32_READ1        0x0AU
#define ESPI_OEM_MEM32_READN        0x0CU

/*
 * The Direct-Request-2 reply carries the x4..x17 registers
 * (struct ffa_send_direct_data2 == unsigned long data[14] == 112 bytes on
 * arm64). The whole span is usable for returned bytes, but keep reads
 * small: the partition rejects oversized requests with "Input or output
 * size is too large".
 */
#define ESPI_MEM32_MAX_COUNT        112U

/* eSPI-mapped addresses of interest (see file header). */
#define ESPI_ADDR_ACTIVE_TABLE_PTR  0x06000e70U
#define ESPI_ADDR_OVERRIDE_LOW      0x06000190U

/* Values the active-table pointer can hold, per firmware disassembly. */
#define EC_THERMAL_TABLE_PROFILE_A  0x000d0fd4U /* boot flag @0x11ab4a == 1 */
#define EC_THERMAL_TABLE_PROFILE_B  0x000d0fe8U /* boot flag @0x11ab4a == 0 */

struct ecpeek_state {
	struct ffa_device *fdev;
	struct mutex lock;
	u32 addr;
	u32 count;
};

/*
 * Issue one OEM1 Memory32 read of @count bytes at eSPI @addr, copying the
 * little-endian reply bytes into @out (which must hold at least @count
 * bytes). Returns 0 on a successful transport round-trip; note an eSPI
 * bus error is reported by the partition as all-zero data, not as an
 * error return.
 */
static int espi_mem32_read(struct ffa_device *fdev, u32 addr, u32 count,
			   u8 *out)
{
	struct ffa_send_direct_data2 msg = {};
	u8 *payload = (u8 *)msg.data;
	int ret;

	if (count == 0 || count > ESPI_MEM32_MAX_COUNT)
		return -EINVAL;

	if (count == 1) {
		payload[0] = ESPI_OEM_MEM32_READ1;
		put_unaligned_le32(addr, &payload[1]);
	} else {
		payload[0] = ESPI_OEM_MEM32_READN;
		put_unaligned_le32(addr, &payload[1]);
		put_unaligned_le32(count, &payload[5]);
	}

	ret = fdev->ops->msg_ops->sync_send_receive2(fdev, &msg);
	if (ret) {
		dev_err(&fdev->dev,
			"FF-A Memory32 read failed: addr=%#010x count=%u ret=%d\n",
			addr, count, ret);
		return ret;
	}

	/* Reply bytes are packed little-endian starting at data[0] byte 0. */
	memcpy(out, payload, count);
	return 0;
}

/* Read a little-endian u32 (4 bytes) at eSPI @addr. */
static int espi_mem32_read_u32(struct ffa_device *fdev, u32 addr, u32 *val)
{
	u8 buf[4];
	int ret;

	ret = espi_mem32_read(fdev, addr, sizeof(buf), buf);
	if (ret)
		return ret;

	*val = get_unaligned_le32(buf);
	return 0;
}

static ssize_t ec_mem32_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct ecpeek_state *state = dev_get_drvdata(dev);
	u8 data[ESPI_MEM32_MAX_COUNT];
	u32 addr, count;
	ssize_t len;
	int ret;

	ret = mutex_lock_interruptible(&state->lock);
	if (ret)
		return ret;

	addr = state->addr;
	count = state->count;
	ret = espi_mem32_read(state->fdev, addr, count, data);

	mutex_unlock(&state->lock);

	if (ret)
		return ret;

	len = sysfs_emit(buf, "%#010x/%u:", addr, count);
	for (u32 i = 0; i < count; i++)
		len += sysfs_emit_at(buf, len, " %02x", data[i]);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t ec_mem32_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct ecpeek_state *state = dev_get_drvdata(dev);
	unsigned int addr, count = 4;
	int n;

	/* Accept "<hexaddr>" or "<hexaddr> <hexcount>"; both are hex. */
	n = sscanf(buf, "%x %x", &addr, &count);
	if (n < 1)
		return -EINVAL;
	if (count == 0 || count > ESPI_MEM32_MAX_COUNT)
		return -EINVAL;

	mutex_lock(&state->lock);
	state->addr = addr;
	state->count = count;
	mutex_unlock(&state->lock);

	return size;
}

static DEVICE_ATTR_RW(ec_mem32);

static ssize_t ec_profile_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ecpeek_state *state = dev_get_drvdata(dev);
	u32 ptr;
	int ret;

	ret = mutex_lock_interruptible(&state->lock);
	if (ret)
		return ret;

	ret = espi_mem32_read_u32(state->fdev, ESPI_ADDR_ACTIVE_TABLE_PTR, &ptr);

	mutex_unlock(&state->lock);

	if (ret)
		return ret;

	switch (ptr) {
	case EC_THERMAL_TABLE_PROFILE_A:
		return sysfs_emit(buf, "A table=%#010x flag=1\n", ptr);
	case EC_THERMAL_TABLE_PROFILE_B:
		return sysfs_emit(buf, "B table=%#010x flag=0\n", ptr);
	default:
		return sysfs_emit(buf, "unknown table=%#010x\n", ptr);
	}
}

static DEVICE_ATTR_RO(ec_profile);

static ssize_t ec_verify_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct ecpeek_state *state = dev_get_drvdata(dev);
	u8 data[2];
	int ret;

	ret = mutex_lock_interruptible(&state->lock);
	if (ret)
		return ret;

	ret = espi_mem32_read(state->fdev, ESPI_ADDR_OVERRIDE_LOW, sizeof(data),
			      data);

	mutex_unlock(&state->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf,
			  "override_low=%#06x (compare to nvfancontrol fan_override 'low')\n",
			  get_unaligned_le16(data));
}

static DEVICE_ATTR_RO(ec_verify);

static struct attribute *ecpeek_attrs[] = {
	&dev_attr_ec_mem32.attr,
	&dev_attr_ec_profile.attr,
	&dev_attr_ec_verify.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ecpeek);

static int ecpeek_probe(struct ffa_device *fdev)
{
	struct ecpeek_state *state;
	int ret;

	if (!fdev->ops || !fdev->ops->msg_ops ||
	    !fdev->ops->msg_ops->sync_send_receive2) {
		dev_err(&fdev->dev, "FF-A Direct Request 2 is unavailable\n");
		return -EOPNOTSUPP;
	}

	if (fdev->mode_32bit) {
		dev_err(&fdev->dev,
			"refusing Direct Request 2 for a 32-bit-mode device\n");
		return -EOPNOTSUPP;
	}

	state = devm_kzalloc(&fdev->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->fdev = fdev;
	state->addr = ESPI_ADDR_ACTIVE_TABLE_PTR;
	state->count = 4;
	mutex_init(&state->lock);
	dev_set_drvdata(&fdev->dev, state);

	ret = device_add_groups(&fdev->dev, ecpeek_groups);
	if (ret) {
		dev_err(&fdev->dev, "failed to create sysfs attributes: %d\n",
			ret);
		return ret;
	}

	dev_info(&fdev->dev,
		 "ecpeek ready: read-only eSPI Memory32; ec_mem32 default addr=%#010x; ec_profile decodes 0x06000e70\n",
		 state->addr);
	return 0;
}

static void ecpeek_remove(struct ffa_device *fdev)
{
	device_remove_groups(&fdev->dev, ecpeek_groups);
}

static const struct ffa_device_id ecpeek_ids[] = {
	{
		.uuid = UUID_INIT(0x884a63a0, 0x3285, 0x4120,
				  0x83, 0xaa, 0xee, 0xc0,
				  0x08, 0xa0, 0xa5, 0x46),
	},
	{},
};

static struct ffa_driver ecpeek_driver = {
	.name = "ecpeek",
	.probe = ecpeek_probe,
	.remove = ecpeek_remove,
	.id_table = ecpeek_ids,
};

module_ffa_driver(ecpeek_driver);

MODULE_DESCRIPTION("DGX Spark EC SRAM peek via FF-A eSPI-service Memory32 read");
MODULE_AUTHOR("Mathieu Lacage");
MODULE_LICENSE("GPL");
