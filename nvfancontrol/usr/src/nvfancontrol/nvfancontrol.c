// SPDX-License-Identifier: GPL-2.0-only
/*
 * DGX Spark DKMS-managed EC fan override / rollback sysfs driver
 *
 * Loading the module only binds the verified FF-A partition and creates:
 *
 *   /sys/bus/arm_ffa/devices/arm-ffa-17/fan            (read/write)
 *   /sys/bus/arm_ffa/devices/arm-ffa-17/fan_caps        (read-only)
 *   /sys/bus/arm_ffa/devices/arm-ffa-17/fan_limits      (read-only)
 *   /sys/bus/arm_ffa/devices/arm-ffa-17/fan_telemetry   (read-only)
 *   /sys/bus/arm_ffa/devices/arm-ffa-17/fan_rpm         (read-only)
 *   /sys/bus/arm_ffa/devices/arm-ffa-17/fan_override    (read/write)
 *
 * Writing "max" or "auto" to "fan" issues one OEM1 command 17 request
 * carrying EC thermal-mailbox inner command 5 (set high override slot).
 * Reading "fan_caps"/"fan_telemetry" issues the same OEM1 command 17
 * transport carrying EC inner command 1 (capabilities/mode/ranges) or 7
 * (64-byte live telemetry snapshot), and returns the raw EC reply bytes
 * as hex text. "fan_limits" issues the same inner command 1 as
 * "fan_caps" and decodes its documented fields: the reported control
 * mode and the minimum/maximum of both fan channels. "fan_rpm" also
 * issues inner command 7 and additionally decodes the two live fan RPM
 * fields found empirically within that snapshot (see the comment above
 * fan_rpm_show()). "fan_override" reads issue inner commands 2 and 4 to
 * read back the EC's two override slots directly, independent of this
 * driver's own last-write cache. "fan_override" writes accept
 * "<low> <high>" (decimal or 0x-prefixed hex, each 0-65535) and issue
 * inner commands 3 and 5 to write both slots (see ec_set_overrides());
 * writing the same value to both pins the fan target there, since the EC
 * apply function clamps its curve output between the low (cap) and high
 * (floor) slots. This is separate from "fan", which only ever writes the
 * high slot. Requests are serialized, and the interface remains available
 * for subsequent requests.
 *
 * Loading also registers a "nvfancontrol_ec" thermal zone and an
 * "nvfancontrol_fan" cooling device in the generic Linux thermal framework
 * (visible under /sys/class/thermal/), but leaves the zone in disabled
 * mode -- consistent with the "no EC request at load" invariant above.
 * Writing "enabled" to that zone's sysfs "mode" attribute hands both
 * override slots to the kernel's step_wise governor, which pins them
 * according to a curve that defaults to mirroring the EC's own curve
 * (tunable via the zone's trip_point_*_temp/_hyst sysfs). While that zone
 * is enabled, "fan" and "fan_override" writes are refused with -EBUSY.
 * failsafe_temp/failsafe_hyst/override_fail_limit module parameters
 * (writable at runtime under /sys/module/nvfancontrol/parameters/) bound
 * how far this can go before overrides are released back to the EC
 * unconditionally.
 */

#include <linux/acpi.h>
#include <linux/arm_ffa.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/hex.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pfn.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>

#define ESPI_OEM_GENERIC_EMI       17U

#define ESPI_NS_SHM_PA             0x933dd000ULL
#define ESPI_NS_SHM_SIZE           0x1000U
#define ESPI_NS_SHM_PROTOCOL_SIZE  32U

#define GENERIC_OUTPUT_OFFSET      0U
#define GENERIC_DATA_OFFSET        0x10U
#define GENERIC_INPUT_ACCEPTED     3U
#define GENERIC_OUTPUT_READY       4U

#define THERMAL_OUTER_COMMAND        0x07U
#define THERMAL_QUERY_CAPS           0x01U
#define THERMAL_QUERY_LOW_OVERRIDE   0x02U
#define THERMAL_SET_LOW_OVERRIDE     0x03U
#define THERMAL_QUERY_HIGH_OVERRIDE  0x04U
#define THERMAL_SET_HIGH_OVERRIDE    0x05U
#define THERMAL_TELEMETRY_SNAPSHOT   0x07U

#define THERMAL_HEADER_LENGTH         3U
#define THERMAL_FAN_EXTRA_LENGTH      2U
#define THERMAL_FAN_REPLY_LENGTH      (THERMAL_HEADER_LENGTH + THERMAL_FAN_EXTRA_LENGTH)
#define THERMAL_CAPS_REPLY_LENGTH     13U
#define THERMAL_TELEMETRY_REPLY_LENGTH 67U
#define THERMAL_MAX_REQUEST_LENGTH    (THERMAL_HEADER_LENGTH + THERMAL_FAN_EXTRA_LENGTH)
#define THERMAL_MAX_REPLY_LENGTH      THERMAL_TELEMETRY_REPLY_LENGTH

/*
 * Layout of the inner-command-1 (capabilities) reply, documented in the
 * project README: after the 3-byte echoed header come a capability byte
 * and a mode byte, then the minimum and maximum of each fan channel as
 * little-endian u16. In mode 0 those four values are RPM; in mode 1 they
 * are PWM percentages.
 */
#define THERMAL_CAPS_MODE_OFFSET      4U
#define THERMAL_CAPS_FAN0_MIN_OFFSET  5U
#define THERMAL_CAPS_FAN0_MAX_OFFSET  7U
#define THERMAL_CAPS_FAN1_MIN_OFFSET  9U
#define THERMAL_CAPS_FAN1_MAX_OFFSET  11U

#define THERMAL_CAPS_MODE_RPM         0U
#define THERMAL_CAPS_MODE_PERCENT     1U

/*
 * Offsets of the two live fan RPM fields (each little-endian u16) within
 * the inner-command-7 reply. Not documented anywhere; determined
 * empirically on an MSI EdgeXpert DGX Spark unit by correlating raw
 * fan_telemetry reads against forced "max"/"auto" transitions and
 * sensors(1) temperatures: both fields ramp from 0 to a steady plateau
 * within ~10s of writing "max" to fan, and ramp smoothly back down over
 * ~70s after writing "auto", tracking each other closely throughout
 * (consistent with two matched fan channels). The steady-state "max"
 * plateau observed (~4332 / ~4444 RPM) is well below the fan0/fan1
 * ranges the README documents (1260-9000 / 1890-13500), which is
 * presumably specific to this OEM's fan hardware rather than a decoding
 * error. Which offset is "fan0" vs "fan1" in the PWM0/TACH0 vs
 * PWM1/TACH1 sense is inferred from position only (lower offset first)
 * and has not been independently confirmed.
 */
#define THERMAL_TELEMETRY_FAN0_RPM_OFFSET 7U
#define THERMAL_TELEMETRY_FAN1_RPM_OFFSET 9U

#define TARGET_FULL_RPM            13500U
#define TARGET_AUTOMATIC           0xffffU

#define REPLY_TIMEOUT_MS           5000U
#define REPLY_POLL_MS              10U
#define ESPI_TIMEOUT_FLOOR_US      40000LL

/*
 * Kernel thermal-zone/cooling-device integration. Five pinned states
 * (0..4) mirror the EC's own 5-level curve percentages, each driven by
 * its own trip point (including state 0, the idle/30C stage); a 6th
 * state releases both override slots back to the EC's native curve.
 */
#define NVFANCONTROL_NUM_PIN_STATES  5U
#define NVFANCONTROL_NUM_TRIPS       NVFANCONTROL_NUM_PIN_STATES
#define NVFANCONTROL_RELEASE_STATE   NVFANCONTROL_NUM_PIN_STATES

/* Upper bound on ACPI ThermalZone objects to track (7 observed on this platform). */
#define NVFANCONTROL_MAX_ACPI_THERMAL 16U

static int failsafe_temp = 100;
module_param(failsafe_temp, int, 0644);
MODULE_PARM_DESC(failsafe_temp,
		 "Release EC overrides immediately once temp reaches this many degrees Celsius (default 100)");

static int failsafe_hyst = 5;
module_param(failsafe_hyst, int, 0644);
MODULE_PARM_DESC(failsafe_hyst,
		 "Degrees C below failsafe_temp before kernel control re-engages after a failsafe release (default 5)");

static int override_fail_limit = 3;
module_param(override_fail_limit, int, 0644);
MODULE_PARM_DESC(override_fail_limit,
		 "Consecutive EC override-write failures before disabling the thermal zone (default 3)");

/* EC curve percentages for pinned states 0..4 (30/40/54/75/100%). */
static const u8 nvfancontrol_curve_pct[NVFANCONTROL_NUM_PIN_STATES] = {
	30, 40, 54, 75, 100
};

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

	/*
	 * ACPI Thermal Zone objects (ASL "ThermalZone()"), found once via
	 * acpi_walk_namespace(ACPI_TYPE_THERMAL, ...) and queried directly
	 * via their _TMP method thereafter. This is the same thing
	 * drivers/acpi/thermal.c itself does internally to answer its own
	 * zones' get_temp -- going straight to ACPI sidesteps
	 * thermal_zone_get_zone_by_name()'s -EEXIST refusal (confirmed on
	 * this kernel, 2026-08-27) when multiple registered zones share a
	 * name, which all 7 "acpitz" zones on this platform do.
	 */
	acpi_handle acpi_thermal_handles[NVFANCONTROL_MAX_ACPI_THERMAL];
	unsigned int acpi_thermal_handle_count;
	bool acpi_thermal_resolved;
	struct thermal_zone_device *tz;
	struct thermal_cooling_device *cdev;
	struct notifier_block reboot_nb;

	bool tz_enabled;
	bool failsafe_engaged;
	unsigned long cur_state;
	unsigned int override_fail_count;

	bool ranges_valid;
	u16 fan1_min;
	u16 fan1_max;
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

	dev_dbg(dev, "shared-buffer restore verified (first %u bytes)\n",
		ESPI_NS_SHM_PROTOCOL_SIZE);
}

static int ec_thermal_request(struct nvfancontrol_state *state, u8 inner_cmd,
			       const u8 *extra, u8 extra_len,
			       u8 *reply, u8 reply_len)
{
	struct ffa_device *fdev = state->fdev;
	struct ffa_send_direct_data2 msg = {};
	u8 snapshot[ESPI_NS_SHM_PROTOCOL_SIZE];
	u8 frame[ESPI_NS_SHM_PROTOCOL_SIZE] = {};
	u8 request[THERMAL_MAX_REQUEST_LENGTH];
	u8 request_len = THERMAL_HEADER_LENGTH + extra_len;
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

	if (WARN_ON(request_len > sizeof(request) ||
		    reply_len > THERMAL_MAX_REPLY_LENGTH ||
		    GENERIC_DATA_OFFSET + reply_len > ESPI_NS_SHM_SIZE))
		return -EINVAL;

	map_memory = pfn_is_map_memory(pfn);
	if (map_memory)
		reserved_page = PageReserved(pfn_to_page(pfn));

	dev_dbg(&fdev->dev,
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
	dev_dbg(&fdev->dev, "shared pre-state: %*ph\n",
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
	request[1] = inner_cmd;
	request[2] = 0;
	if (extra_len)
		memcpy(&request[THERMAL_HEADER_LENGTH], extra, extra_len);

	frame[0] = request_len;
	frame[1] = reply_len;
	frame[2] = GENERIC_OUTPUT_OFFSET;
	memcpy(&frame[GENERIC_DATA_OFFSET], request, request_len);
	memcpy(shm, frame, sizeof(frame));
	mb();

	dev_dbg(&fdev->dev,
		"starting inner-command %#04x request: fixed EC frame %*ph\n",
		inner_cmd, (int)request_len, request);

	payload[0] = ESPI_OEM_GENERIC_EMI;
	start = ktime_get();
	ret = fdev->ops->msg_ops->sync_send_receive2(fdev, &msg);
	elapsed_us = ktime_us_delta(ktime_get(), start);
	if (ret) {
		dev_crit(&fdev->dev,
			 "FF-A TRANSPORT FAILURE: ret=%d elapsed=%lld us; EC state is unknown\n",
			 ret, elapsed_us);
		restore_shared_page(&fdev->dev, shm, snapshot, &ret);
		goto out_unmap;
	}

	raw_response[0] = msg.data[0];
	raw_response[1] = msg.data[1];
	raw_response[2] = msg.data[2];
	raw_response[3] = msg.data[3];
	service_status = get_unaligned_le32((u8 *)msg.data);
	dev_dbg(&fdev->dev,
		"command-17 response: status=%u elapsed=%lld us raw=%#018lx %#018lx %#018lx %#018lx\n",
		service_status, elapsed_us, raw_response[0], raw_response[1],
		raw_response[2], raw_response[3]);

	if (service_status != 0) {
		if (service_status == 5 && elapsed_us >= ESPI_TIMEOUT_FLOOR_US)
			dev_crit(&fdev->dev,
				 "eSPI TIMEOUT: internal mailbox operation timed out; EC state is unknown\n");
		else
			dev_crit(&fdev->dev,
				 "SERVICE REJECTED: status=%u; EC state is unknown\n",
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
			 "OUTPUT TIMEOUT after %u ms: accepted=%#04x ready=%#04x; EC state is unknown; shared frame is left intact and no retry is allowed\n",
			 REPLY_TIMEOUT_MS,
			 READ_ONCE(shm[GENERIC_INPUT_ACCEPTED]),
			 READ_ONCE(shm[GENERIC_OUTPUT_READY]));
		ret = -ETIMEDOUT;
		goto out_unmap;
	}

	mb();
	memcpy(reply, &shm[GENERIC_DATA_OFFSET], reply_len);
	dev_dbg(&fdev->dev,
		"EC reply after <=%u ms: accepted=%#04x ready=%#04x response=%*ph\n",
		elapsed, READ_ONCE(shm[GENERIC_INPUT_ACCEPTED]),
		READ_ONCE(shm[GENERIC_OUTPUT_READY]),
		(int)reply_len, reply);

	if (reply_len < request_len || memcmp(reply, request, request_len)) {
		dev_crit(&fdev->dev,
			 "RESPONSE MISMATCH: expected-prefix=%*ph observed=%*ph; EC state is unknown\n",
			 (int)request_len, request,
			 (int)reply_len, reply);
		result = -EPROTO;
		restore_shared_page(&fdev->dev, shm, snapshot, &result);
		ret = result;
		goto out_unmap;
	}

	dev_dbg(&fdev->dev, "inner-command %#04x ACKNOWLEDGED\n", inner_cmd);

	result = 0;
	restore_shared_page(&fdev->dev, shm, snapshot, &result);
	ret = result;

out_unmap:
	memunmap(shm);
	return ret;
}

static int submit_fan_request(struct nvfancontrol_state *state, u16 target)
{
	u8 extra[THERMAL_FAN_EXTRA_LENGTH];
	u8 reply[THERMAL_FAN_REPLY_LENGTH];
	int ret;

	put_unaligned_le16(target, extra);

	if (target == TARGET_FULL_RPM)
		dev_warn(&state->fdev->dev, "starting MAX request: target=%u\n",
			 target);
	else
		dev_warn(&state->fdev->dev, "starting AUTO request: target=%#x\n",
			 target);

	ret = ec_thermal_request(state, THERMAL_SET_HIGH_OVERRIDE, extra,
				  sizeof(extra), reply, sizeof(reply));
	if (ret)
		return ret;

	if (target == TARGET_FULL_RPM)
		dev_warn(&state->fdev->dev,
			 "MAX ACKNOWLEDGED: EC accepted high override=13500 RPM; fan0 and fan1 policy outputs saturate at 100%%\n");
	else
		dev_warn(&state->fdev->dev,
			 "AUTO ACKNOWLEDGED: EC accepted high override=0xffff; automatic thermal curve is restored\n");

	return 0;
}

/*
 * Writes both EC override slots. The EC apply function clamps its curve
 * output between the low (cap) and high (floor) slots, so writing the same
 * value to both pins the fan target exactly there. If the second write
 * fails after the first succeeded, best-effort restore the low slot to
 * 0xFFFF (disabled) rather than leave a stray cap in place; a failure of
 * that restore is logged at dev_crit since EC override state is then
 * genuinely unknown to the host.
 */
static int ec_set_overrides(struct nvfancontrol_state *state, u16 low, u16 high)
{
	u8 extra[THERMAL_FAN_EXTRA_LENGTH];
	u8 reply[THERMAL_FAN_REPLY_LENGTH];
	int ret;

	put_unaligned_le16(low, extra);
	ret = ec_thermal_request(state, THERMAL_SET_LOW_OVERRIDE, extra,
				  sizeof(extra), reply, sizeof(reply));
	if (ret)
		return ret;

	put_unaligned_le16(high, extra);
	ret = ec_thermal_request(state, THERMAL_SET_HIGH_OVERRIDE, extra,
				  sizeof(extra), reply, sizeof(reply));
	if (ret) {
		int restore_ret;

		put_unaligned_le16(TARGET_AUTOMATIC, extra);
		restore_ret = ec_thermal_request(state, THERMAL_SET_LOW_OVERRIDE,
						  extra, sizeof(extra), reply,
						  sizeof(reply));
		if (restore_ret)
			dev_crit(&state->fdev->dev,
				 "failed to restore low override to 0xffff after high-override failure (restore ret=%d); EC override state is unknown\n",
				 restore_ret);
		return ret;
	}

	return 0;
}

/*
 * Best-effort override release, shared by change_mode(DISABLED), remove(),
 * and the reboot/shutdown notifier. A write failure here is logged but not
 * otherwise handled: there is nothing more this driver can do if the EC
 * itself is not responding at this point.
 */
static void nvfancontrol_release(struct nvfancontrol_state *state, const char *why)
{
	int ret;

	mutex_lock(&state->request_lock);
	ret = ec_set_overrides(state, TARGET_AUTOMATIC, TARGET_AUTOMATIC);
	if (!ret)
		state->cur_state = NVFANCONTROL_RELEASE_STATE;
	mutex_unlock(&state->request_lock);

	if (ret)
		dev_crit(&state->fdev->dev,
			 "%s: failed to release EC overrides (ret=%d); EC override state is unknown\n",
			 why, ret);
	else
		dev_warn(&state->fdev->dev,
			 "%s: EC overrides released (0xffff/0xffff)\n", why);
}

static int nvfancontrol_reboot_notify(struct notifier_block *nb,
				       unsigned long action, void *data)
{
	struct nvfancontrol_state *state =
		container_of(nb, struct nvfancontrol_state, reboot_nb);

	nvfancontrol_release(state, "reboot/shutdown notifier");

	return NOTIFY_DONE;
}

/*
 * Immediate, ungoverned safety valve: independent of whatever the step_wise
 * governor's trip evaluation is doing, force both slots back to automatic
 * the moment temp reaches failsafe_temp, and hold there (set_cur_state()
 * checks failsafe_engaged and clamps any requested state to the release
 * state) until temp drops back below failsafe_temp - failsafe_hyst. Called
 * from get_temp(), so this runs on every poll.
 */
static void nvfancontrol_failsafe_check(struct nvfancontrol_state *state, int temp_mc)
{
	int threshold_mc = failsafe_temp * 1000;
	int clear_mc = (failsafe_temp - failsafe_hyst) * 1000;

	if (!READ_ONCE(state->failsafe_engaged) && temp_mc >= threshold_mc) {
		int ret;

		mutex_lock(&state->request_lock);
		ret = ec_set_overrides(state, TARGET_AUTOMATIC, TARGET_AUTOMATIC);
		if (!ret)
			state->cur_state = NVFANCONTROL_RELEASE_STATE;
		mutex_unlock(&state->request_lock);

		WRITE_ONCE(state->failsafe_engaged, true);
		dev_crit(&state->fdev->dev,
			 "FAILSAFE: temp %d.%03dC >= %dC, released EC overrides%s\n",
			 temp_mc / 1000, temp_mc % 1000, failsafe_temp,
			 ret ? " (release write itself failed; EC state uncertain)" : "");
	} else if (READ_ONCE(state->failsafe_engaged) && temp_mc < clear_mc) {
		WRITE_ONCE(state->failsafe_engaged, false);
		dev_warn(&state->fdev->dev,
			 "failsafe cleared: temp %d.%03dC < %dC, kernel control re-engaging\n",
			 temp_mc / 1000, temp_mc % 1000,
			 failsafe_temp - failsafe_hyst);
	}
}

static acpi_status nvfancontrol_acpi_thermal_found(acpi_handle handle, u32 level,
						     void *context, void **return_value)
{
	struct nvfancontrol_state *state = context;

	if (state->acpi_thermal_handle_count >=
	    ARRAY_SIZE(state->acpi_thermal_handles))
		return AE_CTRL_TERMINATE;

	state->acpi_thermal_handles[state->acpi_thermal_handle_count++] = handle;

	return AE_OK;
}

/* Walk the ACPI namespace for ThermalZone objects once; cache the handles. */
static int nvfancontrol_resolve_acpi_thermal(struct nvfancontrol_state *state)
{
	acpi_status status;

	if (state->acpi_thermal_resolved)
		return state->acpi_thermal_handle_count ? 0 : -ENODEV;

	status = acpi_walk_namespace(ACPI_TYPE_THERMAL, ACPI_ROOT_OBJECT,
				      ACPI_UINT32_MAX,
				      nvfancontrol_acpi_thermal_found, NULL,
				      state, NULL);
	state->acpi_thermal_resolved = true;

	if (state->acpi_thermal_handle_count == 0) {
		dev_err(&state->fdev->dev,
			"acpi_walk_namespace(ACPI_TYPE_THERMAL) found no ThermalZone objects (status=%#x)\n",
			status);
		return -ENODEV;
	}

	dev_info(&state->fdev->dev,
		 "resolved %u ACPI ThermalZone object(s) for temperature (max of _TMP across all)\n",
		 state->acpi_thermal_handle_count);

	return 0;
}

/*
 * Reads _TMP directly on every ACPI ThermalZone object found on this
 * platform and returns the max, in millidegrees C. No FF-A transaction;
 * only actual fan-setpoint changes touch the EC mailbox. This is the same
 * ACPICA call drivers/acpi/thermal.c itself makes internally -- concurrent
 * _TMP evaluation from multiple callers is safe, ACPICA serializes it.
 */
static int nvfancontrol_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct nvfancontrol_state *state = thermal_zone_device_priv(tz);
	int max_mc = INT_MIN;
	unsigned int i, valid = 0;
	int ret;

	ret = nvfancontrol_resolve_acpi_thermal(state);
	if (ret)
		return ret;

	for (i = 0; i < state->acpi_thermal_handle_count; i++) {
		unsigned long long decikelvin;
		acpi_status status;
		int mc;

		status = acpi_evaluate_integer(state->acpi_thermal_handles[i],
						"_TMP", NULL, &decikelvin);
		if (ACPI_FAILURE(status))
			continue;

		/* _TMP is in tenths of Kelvin; millidegrees C = dK*100 - 273150. */
		mc = (int)decikelvin * 100 - 273150;
		if (mc > max_mc)
			max_mc = mc;
		valid++;
	}

	if (!valid)
		return -EIO;

	*temp = max_mc;

	nvfancontrol_failsafe_check(state, *temp);

	return 0;
}

static bool nvfancontrol_should_bind(struct thermal_zone_device *tz,
				      const struct thermal_trip *trip,
				      struct thermal_cooling_device *cdev,
				      struct cooling_spec *cspec)
{
	struct nvfancontrol_state *state = thermal_zone_device_priv(tz);
	unsigned long target;

	if (cdev != state->cdev)
		return false;

	target = (unsigned long)THERMAL_TRIP_PRIV_TO_INT(trip->priv);
	cspec->upper = target;
	cspec->lower = target;
	cspec->weight = 0;

	return true;
}

static int nvfancontrol_change_mode(struct thermal_zone_device *tz,
				     enum thermal_device_mode mode)
{
	struct nvfancontrol_state *state = thermal_zone_device_priv(tz);

	if (mode == THERMAL_DEVICE_DISABLED) {
		nvfancontrol_release(state, "thermal zone disabled");
		WRITE_ONCE(state->tz_enabled, false);
	} else {
		WRITE_ONCE(state->tz_enabled, true);
	}

	return 0;
}

static const struct thermal_zone_device_ops nvfancontrol_tz_ops = {
	.get_temp = nvfancontrol_get_temp,
	.should_bind = nvfancontrol_should_bind,
	.change_mode = nvfancontrol_change_mode,
};

/* Read fan1's control-domain range once (caps cmd 1); cached thereafter. */
static int ensure_ranges(struct nvfancontrol_state *state)
{
	u8 reply[THERMAL_CAPS_REPLY_LENGTH];
	int ret;

	if (state->ranges_valid)
		return 0;

	ret = ec_thermal_request(state, THERMAL_QUERY_CAPS, NULL, 0, reply,
				  sizeof(reply));
	if (ret)
		return ret;

	state->fan1_min = get_unaligned_le16(&reply[THERMAL_CAPS_FAN1_MIN_OFFSET]);
	state->fan1_max = get_unaligned_le16(&reply[THERMAL_CAPS_FAN1_MAX_OFFSET]);
	state->ranges_valid = true;

	return 0;
}

/*
 * Linearly interpolate a pinned state's EC curve percentage over fan1's
 * control-domain range. Phase A verification (2026-08-27) found this
 * control-domain value does not equal the real measured fan_rpm 1:1 on
 * this unit (fan_rpm converged to a value proportional to, but well above,
 * a near-max pinned value) -- that's expected and doesn't matter here,
 * since this writes the same kind of value the EC's own curve would.
 */
static u16 state_pin_value(const struct nvfancontrol_state *state,
			    unsigned long pin_state)
{
	u32 pct = nvfancontrol_curve_pct[pin_state];
	u32 span = state->fan1_max - state->fan1_min;

	return (u16)(state->fan1_min + DIV_ROUND_CLOSEST(pct * span, 100U));
}

static int nvfancontrol_apply_state(struct nvfancontrol_state *state,
				     unsigned long new_state)
{
	u16 low, high;
	int ret;

	if (new_state == NVFANCONTROL_RELEASE_STATE) {
		low = TARGET_AUTOMATIC;
		high = TARGET_AUTOMATIC;
	} else {
		ret = ensure_ranges(state);
		if (ret)
			return ret;
		low = high = state_pin_value(state, new_state);
	}

	return ec_set_overrides(state, low, high);
}

static int nvfancontrol_set_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long new_state)
{
	struct nvfancontrol_state *state = cdev->devdata;
	bool disable_needed = false;
	int ret;

	if (new_state > NVFANCONTROL_RELEASE_STATE)
		return -EINVAL;

	if (READ_ONCE(state->failsafe_engaged))
		new_state = NVFANCONTROL_RELEASE_STATE;

	mutex_lock(&state->request_lock);

	if (new_state == state->cur_state) {
		mutex_unlock(&state->request_lock);
		return 0;
	}

	ret = nvfancontrol_apply_state(state, new_state);
	if (ret) {
		state->override_fail_count++;
		dev_crit(&state->fdev->dev,
			 "cooling state %lu -> %lu failed: ret=%d (consecutive failures=%u/%u)\n",
			 state->cur_state, new_state, ret,
			 state->override_fail_count,
			 (unsigned int)override_fail_limit);
		if (state->override_fail_count >= override_fail_limit)
			disable_needed = true;
	} else {
		dev_info(&state->fdev->dev, "cooling state %lu -> %lu\n",
			 state->cur_state, new_state);
		state->cur_state = new_state;
		state->override_fail_count = 0;
	}

	mutex_unlock(&state->request_lock);

	/*
	 * Disabling must happen outside request_lock: thermal_zone_device_
	 * disable() synchronously calls change_mode(), which takes
	 * request_lock itself.
	 */
	if (disable_needed) {
		dev_crit(&state->fdev->dev,
			 "%u consecutive override-write failures; disabling thermal zone\n",
			 (unsigned int)override_fail_limit);
		thermal_zone_device_disable(state->tz);
	}

	return ret;
}

static int nvfancontrol_get_max_state(struct thermal_cooling_device *cdev,
				       unsigned long *st)
{
	*st = NVFANCONTROL_RELEASE_STATE;
	return 0;
}

static int nvfancontrol_get_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long *st)
{
	struct nvfancontrol_state *state = cdev->devdata;

	*st = READ_ONCE(state->cur_state);
	return 0;
}

static const struct thermal_cooling_device_ops nvfancontrol_cdev_ops = {
	.get_max_state = nvfancontrol_get_max_state,
	.get_cur_state = nvfancontrol_get_cur_state,
	.set_cur_state = nvfancontrol_set_cur_state,
};

/*
 * Trips mirror the EC's own curve levels, including idle (state 0, 30%).
 * priv encodes the pinned state each trip drives (should_bind() reads it
 * back), so should_bind() doesn't need to do pointer arithmetic against
 * this array.
 */
static const struct thermal_trip nvfancontrol_trips[NVFANCONTROL_NUM_TRIPS] = {
	{ .temperature = 30000, .hysteresis = 15000, .type = THERMAL_TRIP_ACTIVE,
	  .flags = THERMAL_TRIP_FLAG_RW, .priv = THERMAL_INT_TO_TRIP_PRIV(0) },
	{ .temperature = 75000, .hysteresis = 10000, .type = THERMAL_TRIP_ACTIVE,
	  .flags = THERMAL_TRIP_FLAG_RW, .priv = THERMAL_INT_TO_TRIP_PRIV(1) },
	{ .temperature = 89000, .hysteresis = 10000, .type = THERMAL_TRIP_ACTIVE,
	  .flags = THERMAL_TRIP_FLAG_RW, .priv = THERMAL_INT_TO_TRIP_PRIV(2) },
	{ .temperature = 95000, .hysteresis = 20000, .type = THERMAL_TRIP_ACTIVE,
	  .flags = THERMAL_TRIP_FLAG_RW, .priv = THERMAL_INT_TO_TRIP_PRIV(3) },
	{ .temperature = 96000, .hysteresis = 20000, .type = THERMAL_TRIP_ACTIVE,
	  .flags = THERMAL_TRIP_FLAG_RW, .priv = THERMAL_INT_TO_TRIP_PRIV(4) },
};

static const struct thermal_zone_params nvfancontrol_tzp = {
	.governor_name = "step_wise",
	.no_hwmon = true,
};

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

	if (READ_ONCE(state->tz_enabled)) {
		dev_err(dev,
			"fan is busy: the nvfancontrol_ec thermal zone is enabled and owns both override slots; disable it first\n");
		return -EBUSY;
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

/*
 * sysfs_emit()'s "%*phN" goes through vsnprintf()'s hex_string(), which
 * silently caps its output at 64 source bytes. THERMAL_TELEMETRY_REPLY_LENGTH
 * is 67, so that path would truncate the reply; bin2hex() has no such cap.
 */
static ssize_t emit_hex(char *buf, const u8 *data, size_t len)
{
	bin2hex(buf, data, len);
	buf[len * 2] = '\n';
	buf[len * 2 + 1] = '\0';
	return len * 2 + 1;
}

static ssize_t fan_caps_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct nvfancontrol_state *state = dev_get_drvdata(dev);
	u8 reply[THERMAL_CAPS_REPLY_LENGTH];
	int ret;

	ret = mutex_lock_interruptible(&state->request_lock);
	if (ret)
		return ret;

	ret = ec_thermal_request(state, THERMAL_QUERY_CAPS, NULL, 0,
				  reply, sizeof(reply));

	mutex_unlock(&state->request_lock);

	if (ret)
		return ret;

	return emit_hex(buf, reply, sizeof(reply));
}

static DEVICE_ATTR_RO(fan_caps);

static const char *caps_mode_name(u8 mode)
{
	switch (mode) {
	case THERMAL_CAPS_MODE_RPM:
		return "rpm";
	case THERMAL_CAPS_MODE_PERCENT:
		return "percent";
	default:
		return "unknown";
	}
}

static ssize_t fan_limits_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct nvfancontrol_state *state = dev_get_drvdata(dev);
	u8 reply[THERMAL_CAPS_REPLY_LENGTH];
	u16 fan0_min, fan0_max, fan1_min, fan1_max;
	u8 mode;
	int ret;

	ret = mutex_lock_interruptible(&state->request_lock);
	if (ret)
		return ret;

	ret = ec_thermal_request(state, THERMAL_QUERY_CAPS, NULL, 0,
				  reply, sizeof(reply));

	mutex_unlock(&state->request_lock);

	if (ret)
		return ret;

	mode = reply[THERMAL_CAPS_MODE_OFFSET];
	fan0_min = get_unaligned_le16(&reply[THERMAL_CAPS_FAN0_MIN_OFFSET]);
	fan0_max = get_unaligned_le16(&reply[THERMAL_CAPS_FAN0_MAX_OFFSET]);
	fan1_min = get_unaligned_le16(&reply[THERMAL_CAPS_FAN1_MIN_OFFSET]);
	fan1_max = get_unaligned_le16(&reply[THERMAL_CAPS_FAN1_MAX_OFFSET]);

	return sysfs_emit(buf,
			   "mode=%s(%u) fan0_min=%u fan0_max=%u fan1_min=%u fan1_max=%u\n",
			   caps_mode_name(mode), mode, fan0_min, fan0_max,
			   fan1_min, fan1_max);
}

static DEVICE_ATTR_RO(fan_limits);

static ssize_t fan_telemetry_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nvfancontrol_state *state = dev_get_drvdata(dev);
	u8 reply[THERMAL_TELEMETRY_REPLY_LENGTH];
	int ret;

	ret = mutex_lock_interruptible(&state->request_lock);
	if (ret)
		return ret;

	ret = ec_thermal_request(state, THERMAL_TELEMETRY_SNAPSHOT, NULL, 0,
				  reply, sizeof(reply));

	mutex_unlock(&state->request_lock);

	if (ret)
		return ret;

	return emit_hex(buf, reply, sizeof(reply));
}

static DEVICE_ATTR_RO(fan_telemetry);

static ssize_t fan_rpm_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct nvfancontrol_state *state = dev_get_drvdata(dev);
	u8 reply[THERMAL_TELEMETRY_REPLY_LENGTH];
	u16 fan0_rpm, fan1_rpm;
	int ret;

	ret = mutex_lock_interruptible(&state->request_lock);
	if (ret)
		return ret;

	ret = ec_thermal_request(state, THERMAL_TELEMETRY_SNAPSHOT, NULL, 0,
				  reply, sizeof(reply));

	mutex_unlock(&state->request_lock);

	if (ret)
		return ret;

	fan0_rpm = get_unaligned_le16(&reply[THERMAL_TELEMETRY_FAN0_RPM_OFFSET]);
	fan1_rpm = get_unaligned_le16(&reply[THERMAL_TELEMETRY_FAN1_RPM_OFFSET]);

	return sysfs_emit(buf, "fan0_rpm=%u fan1_rpm=%u\n", fan0_rpm, fan1_rpm);
}

static DEVICE_ATTR_RO(fan_rpm);

static ssize_t fan_override_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct nvfancontrol_state *state = dev_get_drvdata(dev);
	u8 low_reply[THERMAL_FAN_REPLY_LENGTH];
	u8 high_reply[THERMAL_FAN_REPLY_LENGTH];
	u16 low, high;
	int ret;

	ret = mutex_lock_interruptible(&state->request_lock);
	if (ret)
		return ret;

	ret = ec_thermal_request(state, THERMAL_QUERY_LOW_OVERRIDE, NULL, 0,
				  low_reply, sizeof(low_reply));
	if (!ret)
		ret = ec_thermal_request(state, THERMAL_QUERY_HIGH_OVERRIDE,
					  NULL, 0, high_reply,
					  sizeof(high_reply));

	mutex_unlock(&state->request_lock);

	if (ret)
		return ret;

	low = get_unaligned_le16(&low_reply[THERMAL_HEADER_LENGTH]);
	high = get_unaligned_le16(&high_reply[THERMAL_HEADER_LENGTH]);

	return sysfs_emit(buf, "low=%#06x(%u)%s high=%#06x(%u)%s\n",
			   low, low, low == TARGET_AUTOMATIC ? " disabled" : "",
			   high, high,
			   high == TARGET_AUTOMATIC ? " disabled" : "");
}

/*
 * Testing-only direct override write, ahead of the thermal-zone/cooling-
 * device integration that will normally own these slots. Accepts
 * "<low> <high>" (each decimal or 0x-prefixed hex, 0-65535); writing the
 * same value to both pins the fan target there (see ec_set_overrides()).
 */
static ssize_t fan_override_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct nvfancontrol_state *state = dev_get_drvdata(dev);
	unsigned int low_val, high_val;
	int ret;

	if (sscanf(buf, "%i %i", &low_val, &high_val) != 2 ||
	    low_val > U16_MAX || high_val > U16_MAX) {
		dev_err(dev,
			"fan_override accepts \"<low> <high>\" (decimal or 0x.., each 0-65535)\n");
		return -EINVAL;
	}

	if (READ_ONCE(state->tz_enabled)) {
		dev_err(dev,
			"fan_override is busy: the nvfancontrol_ec thermal zone is enabled and owns both override slots; disable it first\n");
		return -EBUSY;
	}

	ret = mutex_lock_interruptible(&state->request_lock);
	if (ret)
		return ret;

	ret = ec_set_overrides(state, (u16)low_val, (u16)high_val);

	mutex_unlock(&state->request_lock);

	if (ret)
		return ret;

	dev_warn(dev, "fan_override manually set: low=%#06x(%u) high=%#06x(%u)\n",
		 low_val, low_val, high_val, high_val);

	return count;
}

static DEVICE_ATTR_RW(fan_override);

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

	ret = device_create_file(&fdev->dev, &dev_attr_fan_caps);
	if (ret) {
		dev_err(&fdev->dev,
			"failed to create sysfs fan_caps attribute: %d\n", ret);
		goto err_remove_fan;
	}

	ret = device_create_file(&fdev->dev, &dev_attr_fan_telemetry);
	if (ret) {
		dev_err(&fdev->dev,
			"failed to create sysfs fan_telemetry attribute: %d\n",
			ret);
		goto err_remove_fan_caps;
	}

	ret = device_create_file(&fdev->dev, &dev_attr_fan_rpm);
	if (ret) {
		dev_err(&fdev->dev,
			"failed to create sysfs fan_rpm attribute: %d\n", ret);
		goto err_remove_fan_telemetry;
	}

	ret = device_create_file(&fdev->dev, &dev_attr_fan_override);
	if (ret) {
		dev_err(&fdev->dev,
			"failed to create sysfs fan_override attribute: %d\n",
			ret);
		goto err_remove_fan_rpm;
	}

	ret = device_create_file(&fdev->dev, &dev_attr_fan_limits);
	if (ret) {
		dev_err(&fdev->dev,
			"failed to create sysfs fan_limits attribute: %d\n",
			ret);
		goto err_remove_fan_override;
	}

	state->reboot_nb.notifier_call = nvfancontrol_reboot_notify;
	ret = register_reboot_notifier(&state->reboot_nb);
	if (ret) {
		dev_err(&fdev->dev, "failed to register reboot notifier: %d\n",
			ret);
		goto err_remove_fan_limits;
	}

	state->cdev = thermal_cooling_device_register("nvfancontrol_fan", state,
						       &nvfancontrol_cdev_ops);
	if (IS_ERR(state->cdev)) {
		ret = PTR_ERR(state->cdev);
		state->cdev = NULL;
		dev_err(&fdev->dev, "failed to register cooling device: %d\n",
			ret);
		goto err_unregister_reboot;
	}

	state->tz = thermal_zone_device_register_with_trips(
			"nvfancontrol_ec", nvfancontrol_trips,
			NVFANCONTROL_NUM_TRIPS, state, &nvfancontrol_tz_ops,
			&nvfancontrol_tzp, 0, 2000);
	if (IS_ERR(state->tz)) {
		ret = PTR_ERR(state->tz);
		state->tz = NULL;
		dev_err(&fdev->dev, "failed to register thermal zone: %d\n",
			ret);
		goto err_unregister_cdev;
	}

	dev_info(&fdev->dev,
		 "sysfs control ready: %s/fan accepts 'max' or 'auto'; fan_caps, fan_limits, fan_telemetry, fan_rpm are read-only, fan_override is writable for testing; nvfancontrol_ec thermal zone registered but disabled (write 'enabled' to its sysfs mode to start kernel fan control); module load issued no EC request\n",
		 dev_name(&fdev->dev));
	return 0;

err_unregister_cdev:
	thermal_cooling_device_unregister(state->cdev);
	state->cdev = NULL;
err_unregister_reboot:
	unregister_reboot_notifier(&state->reboot_nb);
err_remove_fan_limits:
	device_remove_file(&fdev->dev, &dev_attr_fan_limits);
err_remove_fan_override:
	device_remove_file(&fdev->dev, &dev_attr_fan_override);
err_remove_fan_rpm:
	device_remove_file(&fdev->dev, &dev_attr_fan_rpm);
err_remove_fan_telemetry:
	device_remove_file(&fdev->dev, &dev_attr_fan_telemetry);
err_remove_fan_caps:
	device_remove_file(&fdev->dev, &dev_attr_fan_caps);
err_remove_fan:
	device_remove_file(&fdev->dev, &dev_attr_fan);
	return ret;
}

static void fan_override_remove(struct ffa_device *fdev)
{
	struct nvfancontrol_state *state = dev_get_drvdata(&fdev->dev);

	if (state->tz) {
		/*
		 * Disabling (if currently enabled) synchronously calls
		 * change_mode(DISABLED), which releases the overrides;
		 * a no-op if the zone was never enabled.
		 */
		thermal_zone_device_disable(state->tz);
		thermal_zone_device_unregister(state->tz);
	}
	if (state->cdev)
		thermal_cooling_device_unregister(state->cdev);
	unregister_reboot_notifier(&state->reboot_nb);

	device_remove_file(&fdev->dev, &dev_attr_fan_limits);
	device_remove_file(&fdev->dev, &dev_attr_fan_override);
	device_remove_file(&fdev->dev, &dev_attr_fan_rpm);
	device_remove_file(&fdev->dev, &dev_attr_fan_telemetry);
	device_remove_file(&fdev->dev, &dev_attr_fan_caps);
	device_remove_file(&fdev->dev, &dev_attr_fan);
	dev_warn(&fdev->dev,
		 "module removed; the thermal zone's overrides (if it was enabled) were released; unloading otherwise does NOT change the EC override value\n");
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

MODULE_DESCRIPTION("DGX Spark persistent EC fan sysfs controller and telemetry reader");
MODULE_AUTHOR("841973620");
MODULE_LICENSE("GPL");
