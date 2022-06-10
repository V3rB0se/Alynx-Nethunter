/*
 * Copyright 2016-2017 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/extcon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pmic-voter.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/rtc.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/usb/typec.h>
#include <linux/usb/pd.h>
#include <linux/usb/tcpm.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "logbuffer.h"
#include "usbpd.h"

#define LOG_BUFFER_ENTRIES	1024
#define LOG_BUFFER_ENTRY_SIZE	256

#define EXT_VBUS_WORK_DELAY_MS 5000
#define EXT_VBUS_OVERLAP_MS       5

#define OTG_ICL_VOTER "OTG_ICL_VOTER"
#define OTG_DISABLE_APSD_VOTER "OTG_DISABLE_APSD_VOTER"
#define DISABLE_CC_VOTER "DISABLE_CC_VOTER"

struct usbpd {
	struct device		dev;

	struct tcpm_port	*tcpm_port;
	struct tcpc_dev		tcpc_dev;

	struct pd_phy_params	pdphy_params;
	bool			pdphy_open;

	struct extcon_dev	*extcon;

	struct power_supply	*usb_psy;
	struct power_supply	*wireless_psy;
	struct notifier_block	psy_nb;

	struct regulator	*vbus;
	struct regulator	*vconn;
	struct regulator        *ext_vbus;
	bool			vbus_output;
	bool			external_vbus;
	bool			external_vbus_update;
	bool			vconn_output;

	bool			vbus_present;
	enum power_supply_type	psy_type;
	enum typec_cc_status	cc1;
	enum typec_cc_status	cc2;
	bool			is_cable_flipped;

	bool			pending_update_usb_data;
	bool			extcon_usb;
	bool			extcon_usb_host;
	bool			extcon_usb_cc;

	struct mutex		lock; /* struct usbpd access lock */
	struct workqueue_struct	*wq;

	enum typec_role cur_pwr_role;
	enum typec_data_role cur_data_role;

	/* Flag gets set when hard_reset is being performed */
	bool in_hard_reset;

	/* Flag gets set when a pd_capable partner is attached */
	bool pd_capable;

	/* debugfs logging */
	struct dentry *rootdir;

	bool in_pr_swap;

	/* Ext vbus management */
	struct delayed_work ext_vbus_work;
	struct notifier_block ext_vbus_nb;
	struct votable *usb_icl_votable;
	struct votable *apsd_disable_votable;

	/* Acutual regulator status */
	bool smb2_vbus_reg;
	bool ext_vbus_reg;

	/* Indicates whether the device has to honor usb suspend power limits*/
	bool suspend_supported;
	bool usb_comm_capable;

	bool apsd_done;
	bool wlc_supported;
	bool wireless_online;
	bool in_explicit_contract;

	/* alternate source capabilities */
	struct work_struct update_pdo_work;
	bool default_src_cap;

	struct votable *disable_pr_switch;

	/* logging client */
	struct logbuffer *log;
};

static int pd_engine_debugfs_init(struct usbpd *pd)
{
	if (!pd->rootdir) {
		pd->rootdir = debugfs_create_dir("pd_engine", NULL);
		if (!pd->rootdir)
			return -ENOMEM;
	}

	return 0;
}

static void pd_engine_debugfs_exit(struct usbpd *pd)
{
	debugfs_remove_recursive(pd->rootdir);
}

static const char * const get_typec_mode_name(
		enum power_supply_typec_mode typec_mode)
{
	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_NONE:
		return "NONE";
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		return "SOURCE_DEFAULT";
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		return "SOURCE_MEDIUM";
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		return "SOURCE_HIGH";
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
		return "NON_COMPLIANT";
	case POWER_SUPPLY_TYPEC_SINK:
		return "SINK";
	case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
		return "SINK_POWERED_CABLE";
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
		return "SINK_DEBUG_ACCESSORY";
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
		return "SINK_AUDIO_ADAPTER";
	case POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY:
		return "POWERED_CABLE_ONLY";
	default:
		return "UNDEFINED";
	}
}

static const char * const get_psy_type_name(enum power_supply_type psy_type)
{
	switch (psy_type) {
	case POWER_SUPPLY_TYPE_UNKNOWN:
		return "UNKNOWN";
	case POWER_SUPPLY_TYPE_USB:
		return "SDP";
	case POWER_SUPPLY_TYPE_USB_CDP:
		return "CDP";
	case POWER_SUPPLY_TYPE_USB_DCP:
		return "DCP";
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		return "HVDCP2";
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		return "HVDCP3";
	case POWER_SUPPLY_TYPE_USB_PD:
		return "PD";
	default:
		return "UNDEFINED";
	}
}

/* must align with smblib's integer representation of cc orientation. */
enum typec_cc_orientation {
	TYPEC_CC_ORIENTATION_NONE = 0,
	TYPEC_CC_ORIENTATION_CC1 = 1,
	TYPEC_CC_ORIENTATION_CC2 = 2,
};

static const char * const get_typec_cc_orientation_name(
		enum typec_cc_orientation typec_cc_orientation)
{
	switch (typec_cc_orientation) {
	case TYPEC_CC_ORIENTATION_NONE:
		return "NONE";
	case TYPEC_CC_ORIENTATION_CC1:
		return "CC1";
	case TYPEC_CC_ORIENTATION_CC2:
		return "CC2";
	default:
		return "UNDEFINED";
	}
}

static const char * const get_typec_cc_status_name(
		enum typec_cc_status cc_status)
{
	switch (cc_status) {
	case TYPEC_CC_OPEN:
		return "OPEN";
	case TYPEC_CC_RA:
		return "Ra";
	case TYPEC_CC_RD:
		return "Rd";
	case TYPEC_CC_RP_DEF:
		return "Rd-def";
	case TYPEC_CC_RP_1_5:
		return "Rd-1.5";
	case TYPEC_CC_RP_3_0:
		return "Rd-3.0";
	default:
		return "UNDEFINED";
	}
}

/*
 * parses the type-c mode to cc pin status in the orientation_none special case.
 */
static void parse_cc_status_none_orientation(
		enum power_supply_typec_mode typec_mode,
		enum typec_cc_status *cc1,
		enum typec_cc_status *cc2)
{
	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
		*cc1 = *cc2 = TYPEC_CC_RD;
		break;
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
		*cc1 = *cc2 = TYPEC_CC_RA;
		break;
	default:
		*cc1 = *cc2 = TYPEC_CC_OPEN;
		break;
	}
}

/*
 * parses the type-c mode to the status of the active and inactive cc pin.
 */
static void parse_cc_status_active_inactive(
		enum power_supply_typec_mode typec_mode,
		enum typec_cc_status *cc_active,
		enum typec_cc_status *cc_inactive)
{
	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_NONE:
		*cc_active = TYPEC_CC_OPEN;
		*cc_inactive = TYPEC_CC_OPEN;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		*cc_active = TYPEC_CC_RP_DEF;
		*cc_inactive = TYPEC_CC_OPEN;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		*cc_active = TYPEC_CC_RP_1_5;
		*cc_inactive = TYPEC_CC_OPEN;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		*cc_active = TYPEC_CC_RP_3_0;
		*cc_inactive = TYPEC_CC_OPEN;
		break;
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
		*cc_active = TYPEC_CC_OPEN;
		*cc_inactive = TYPEC_CC_OPEN;
		break;
	case POWER_SUPPLY_TYPEC_SINK:
		*cc_active = TYPEC_CC_RD;
		*cc_inactive = TYPEC_CC_OPEN;
		break;
	case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
		*cc_active = TYPEC_CC_RD;
		*cc_inactive = TYPEC_CC_RA;
		break;
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
		*cc_active = TYPEC_CC_RD;
		*cc_inactive = TYPEC_CC_RD;
		break;
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
		*cc_active = TYPEC_CC_RA;
		*cc_inactive = TYPEC_CC_RA;
		break;
	case POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY:
		*cc_active = TYPEC_CC_RA;
		*cc_inactive = TYPEC_CC_OPEN;
		break;
	default:
		*cc_active = TYPEC_CC_OPEN;
		*cc_inactive = TYPEC_CC_OPEN;
		break;
	}
}

static void parse_cc_status(enum power_supply_typec_mode typec_mode,
			    enum typec_cc_orientation typec_cc_orientation,
			    enum typec_cc_status *cc1,
			    enum typec_cc_status *cc2)
{
	enum typec_cc_status cc_active, cc_inactive;

	/* handle orientation_none special cases */
	if (typec_cc_orientation == TYPEC_CC_ORIENTATION_NONE) {
		parse_cc_status_none_orientation(typec_mode, cc1, cc2);
		return;
	}

	parse_cc_status_active_inactive(typec_mode, &cc_active, &cc_inactive);

	/* assign cc1, cc2 status based on orientation */
	if (typec_cc_orientation == TYPEC_CC_ORIENTATION_CC1) {
		*cc1 = cc_active;
		*cc2 = cc_inactive;
	} else if (typec_cc_orientation == TYPEC_CC_ORIENTATION_CC2) {
		*cc1 = cc_inactive;
		*cc2 = cc_active;
	} else {
		*cc1 = *cc2 = TYPEC_CC_OPEN;
	}
}

static inline bool psy_support_usb_data(enum power_supply_type psy_type)
{
	return (psy_type == POWER_SUPPLY_TYPE_USB) ||
	       (psy_type == POWER_SUPPLY_TYPE_USB_CDP);
}

static int update_extcon_prop(struct usbpd *pd, int extcon_type)
{
	int ret;
	union extcon_property_value val;

	val.intval = pd->extcon_usb_cc ? 1 : 0;
	ret = extcon_set_property(pd->extcon, extcon_type,
				  EXTCON_PROP_USB_TYPEC_POLARITY,
				  val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set extcon usb polarity prop [%s], ret=%d",
			      pd->extcon_usb_cc ? "Y" : "N", ret);
		return ret;
	}

	/*
	 * When wirless charging is on and attempting to start host mode,
	 * degrade to HS as internal pmic cannot be on.
	 */
	if (pd->wireless_online && extcon_type == EXTCON_USB_HOST)
		val.intval = 0;
	else
		val.intval = 1;

	ret = extcon_set_property(pd->extcon, extcon_type,
				  EXTCON_PROP_USB_SS,
				  val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set extcon usb ss prop, ret=%d",
			      ret);
		return ret;
	}

	return ret;
}

static int update_usb_data_role(struct usbpd *pd)
{
	int ret;
	bool extcon_usb;

	/* Turn on USB data (device role) only when a valid power supply that
	 * supports USB data connection is connected, i.e. SDP or CDP
	 *
	 * update_usb_data_role() is called when APSD is done, thus here
	 * pd->psy_type is a valid detection result.
	 */
	extcon_usb = pd->extcon_usb &&
		     (psy_support_usb_data(pd->psy_type) ||
		      pd->usb_comm_capable);

	/*
	 * EXTCON_USB_HOST and EXTCON_USB is mutually exlusive.
	 * Therefore while responding to commands such as DR_SWAP,
	 * update the cable state of the one being turned off before
	 * turning on the other one.
	 */
	if (!pd->extcon_usb_host) {
		ret = extcon_set_state_sync(pd->extcon, EXTCON_USB_HOST,
					      pd->extcon_usb_host);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "unable to turn off extcon usb host, ret=%d",
				      ret);
			return ret;
		}
	}

	if (!extcon_usb) {
		ret = extcon_set_state_sync(pd->extcon, EXTCON_USB,
					      extcon_usb);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "unable to turn off extcon usb device, ret=%d",
				      ret);
			return ret;
		}
	}

	/*
	 * Shouldnt be turning on extcon_usb and extcon_usb_host
	 * at the same time.
	 */
	BUG_ON(extcon_usb && pd->extcon_usb_host);

	if (extcon_usb || pd->extcon_usb_host) {
		ret = update_extcon_prop(pd, extcon_usb ?
					 EXTCON_USB : EXTCON_USB_HOST);
		if (ret < 0)
			return ret;

		ret = extcon_set_state_sync(pd->extcon, extcon_usb ?
					    EXTCON_USB : EXTCON_USB_HOST,
					    1);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "unable to turn on extcon [%s], ret=%d",
				      extcon_usb ? "device" : "host", ret);
			return ret;
		}
	}

	logbuffer_log(pd->log,
		      "usb extcon: cc [%s], host [%s], usb [%s]",
		      pd->extcon_usb_cc ? "Y" : "N",
		      pd->extcon_usb_host ? "Y" : "N",
		      extcon_usb ? "Y" : "N");

	return ret;
}

struct psy_changed_event {
	struct work_struct work;
	struct usbpd *pd;
};

static int pd_regulator_update(struct usbpd *pd, bool external_reg, bool on)
{
	int ret = 0;

	if (on) {
		ret = regulator_enable(external_reg ? pd->ext_vbus
				       : pd->vbus);
		if (ret) {
			logbuffer_log(pd->log,
				      "pd_regulator_update: unable to turn on %s vbus ret = %d"
				      , external_reg ? "external" : "pmic"
				      , ret);
			return ret;
		} else {
			logbuffer_log(pd->log,
				      "pd_regulator_update: turned on %s vbus ret = %d"
				      , external_reg ? "external" : "pmic"
				      , ret);
		}
	} else {
		ret = regulator_disable(external_reg ? pd->ext_vbus
					: pd->vbus);
		if (ret) {
			logbuffer_log(pd->log,
				      "pd_regulator_update: unable to turn off %s vbus ret = %d"
				      , external_reg ? "external" : "pmic"
				      , ret);
			return ret;
		} else {
			logbuffer_log(pd->log,
				      "pd_regulator_update: turned off %s vbus ret = %d"
				      , external_reg ? "external" : "pmic"
				      , ret);
		}
	}

	if (external_reg)
		pd->ext_vbus_reg = on;
	else
		pd->smb2_vbus_reg = on;

	return ret;
}

static int update_vbus_locked(struct usbpd *pd, bool vbus_output)
{
	int ret = 0;

	if (pd->vbus_output == vbus_output)
		return ret;

	pd->external_vbus = pd->wireless_online ? true : false;

	logbuffer_log(pd->log,
		      "update_vbus_locked: vbus_output: %c wireless_online: %c"
		      , vbus_output ? 'Y' : 'N', pd->wireless_online ?
		      'Y' : 'N');

	schedule_work(&pd->update_pdo_work);

	if (vbus_output) {
		ret = pd_regulator_update(pd, pd->external_vbus, true);
		if (ret)
			return ret;
	} else {
		if (pd->ext_vbus_reg) {
			ret = pd_regulator_update(pd, true, false);
			if (ret)
				return ret;
		}

		if (pd->smb2_vbus_reg) {
			ret = pd_regulator_update(pd, false, false);
			if (ret)
				return ret;
		}
	}
	pd->vbus_output = vbus_output;
	/*
	 * No interrupt will be trigerred for the vbus change.
	 * Manually inform tcpm to check the vbus change.
	 */
	tcpm_vbus_change(pd->tcpm_port);
	return ret;
}

void update_external_vbus(struct work_struct *work)
{
	int ret;
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(to_delayed_work(work), struct usbpd,
					ext_vbus_work);

	mutex_lock(&pd->lock);
	/* Exit when the old value is same as the new value or
	 * update the value and exit when vbus is not on.
	 */
	if (pd->external_vbus == pd->external_vbus_update || !pd->vbus_output) {
		logbuffer_log(pd->log,
			      "skipping update value changed: %c vbus_output: %c"
			      , pd->external_vbus == pd->external_vbus_update ?
			       'Y' : 'N', pd->vbus_output ? 'Y' : 'N');
		goto exit;
	}

	if (pd->wireless_online) {
		logbuffer_log(pd->log,
			      "skipping update as wireless charger online");
		goto err;
	}

	val.intval = 1;
	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_OTG_FASTROLESWAP,
					&val);
	if (ret < 0) {
		logbuffer_log(pd->log, "unable to set FASTROLESWAP, ret=%d",
			      ret);
		goto err;
	}

	/*
	 * Turn on the other regulator before turning off the other one.
	 */
	ret = pd_regulator_update(pd, pd->external_vbus_update, true);
	if (ret)
		goto err;

	/* Regulator overlap according to hardware recommendation */
	msleep(EXT_VBUS_OVERLAP_MS);
	ret = pd_regulator_update(pd, !pd->external_vbus_update, false);
	if (ret)
		goto err;

	val.intval = 0;
	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_OTG_FASTROLESWAP,
					&val);
	if (ret < 0) {
		logbuffer_log(pd->log, "unable to clear FASTROLESWAP, ret=%d",
			      ret);
	}

	logbuffer_log(pd->log, "EXT_VBUS_%s", pd->external_vbus_update ? "ON" :
		      "OFF");
exit:
	pd->external_vbus = pd->external_vbus_update;
	schedule_work(&pd->update_pdo_work);
err:
	pm_relax(&pd->dev);
	mutex_unlock(&pd->lock);
}

#define PDO_FIXED_FLAGS \
	(PDO_FIXED_DUAL_ROLE | PDO_FIXED_DATA_SWAP | PDO_FIXED_USB_COMM)

static void update_src_caps(struct work_struct *work)
{
	struct usbpd *pd = container_of(work, struct usbpd, update_pdo_work);
	struct tcpc_config *config = pd->tcpc_dev.config;
	u32 pdo[1];

	if (pd->wireless_online && pd->default_src_cap) {
		logbuffer_log(pd->log, "alternative src_cap");
		pdo[0] = PDO_FIXED(5000, 500, PDO_FIXED_FLAGS);
		tcpm_update_source_capabilities(pd->tcpm_port, pdo, 1);
		pd->default_src_cap = false;
	} else if (!pd->wireless_online &&
		   !pd->external_vbus &&
		   !pd->default_src_cap) {
		logbuffer_log(pd->log, "default src_cap");
		tcpm_update_source_capabilities(pd->tcpm_port,
						config->src_pdo,
						config->nr_src_pdo);
		pd->default_src_cap = true;
	}
}

static void psy_changed_handler(struct work_struct *work)
{
	struct psy_changed_event *event = container_of(work,
						       struct psy_changed_event,
						       work);
	struct usbpd *pd = event->pd;
	bool vbus_present;
	enum typec_cc_status cc1;
	enum typec_cc_status cc2;

	enum power_supply_type psy_type;
	enum power_supply_typec_mode typec_mode;
	enum typec_cc_orientation typec_cc_orientation;

	bool pe_start;

	union power_supply_propval val;
	bool wireless_online = false;
	int ret = 0;

	pm_wakeup_event(&pd->dev, PD_ACTIVITY_TIMEOUT_MS);
	ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_MODE, &val);
	if (ret < 0) {
		logbuffer_log(pd->log, "Unable to read TYPEC_MODE, ret=%d",
			      ret);
		return;
	}
	typec_mode = val.intval;

	ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PE_START, &val);
	if (ret < 0) {
		logbuffer_log(pd->log, "Unable to read PE_START, ret=%d",
			      ret);
		return;
	}
	pe_start = !!val.intval;

	ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_REAL_TYPE, &val);
	if (ret < 0) {
		logbuffer_log(pd->log, "Unable to read TYPE, ret=%d", ret);
		return;
	}
	psy_type = val.intval;
	pd->apsd_done = !!psy_type;

	ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PRESENT, &val);
	if (ret < 0) {
		logbuffer_log(pd->log, "Unable to read ONLINE, ret=%d",
			      ret);
		return;
	}
	vbus_present = val.intval;

	ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION,
					&val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "Unable to read TYPEC_CC_ORIENTATION, ret=%d",
			      ret);
		return;
	}
	typec_cc_orientation = val.intval;

	if (pd->wlc_supported) {
		ret = power_supply_get_property(pd->wireless_psy,
						POWER_SUPPLY_PROP_ONLINE,
						&val);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "Unable to read wireless online property, ret=%d",
				      ret);
			return;
		}
		wireless_online = val.intval ? true : false;
	}

	parse_cc_status(typec_mode, typec_cc_orientation, &cc1, &cc2);

	logbuffer_log(pd->log,
		      "type [%s], pe_start [%s], vbus_present [%s], mode [%s], orientation [%s], cc1 [%s], cc2 [%s] ext_vbus [%s] wireless_online [%s]",
		      get_psy_type_name(psy_type),
		      pe_start ? "Y" : "N",
		      vbus_present ? "Y" : "N",
		      get_typec_mode_name(typec_mode),
		      get_typec_cc_orientation_name(typec_cc_orientation),
		      get_typec_cc_status_name(cc1),
		      get_typec_cc_status_name(cc2),
		      pd->ext_vbus_reg ? "Y" : "N",
		      wireless_online ? "Y" : "N");

	mutex_lock(&pd->lock);
	if (wireless_online != pd->wireless_online) {
		pd->wireless_online = wireless_online;
		logbuffer_log(pd->log,
			      "pd->vbus_output: %c pd->external_vbus: %c",
			      pd->vbus_output ? 'Y' : 'N',
			      pd->external_vbus ? 'Y' : 'N');

		/* Setting different src_cap depending on wireless status */
		schedule_work(&pd->update_pdo_work);

		if (pd->vbus_output && !pd->external_vbus) {
			/* Turn off internal vbus */
			if (pd_regulator_update(pd, false, false))
				goto unlock;
			pd->vbus_output = false;

			/* Force disconnect */
			tcpm_port_reset(pd->tcpm_port);

			goto unlock;
		}
	}

	/* Dont proceed as pmi might still be evaluating connections */
	if (!pe_start && (typec_mode != POWER_SUPPLY_TYPEC_NONE)) {
		logbuffer_log(pd->log,
			      "Skipping update as PE_START not set yet");
		goto unlock;
	}

	pd->psy_type = psy_type;
	pd->is_cable_flipped = typec_cc_orientation == TYPEC_CC_ORIENTATION_CC2;
	pd->extcon_usb_cc = pd->is_cable_flipped;

	if (vbus_present != pd->vbus_present) {
		logbuffer_log(pd->log, "vbus present: %s -> %s",
			      pd->vbus_present ? "True" : "False",
			      vbus_present ? "True" : "False");
		pd->vbus_present = vbus_present;
		tcpm_vbus_change(pd->tcpm_port);
	}

	if (cc1 != pd->cc1 || cc2 != pd->cc2) {
		logbuffer_log(pd->log, "cc1: %s -> %s, cc2: %s -> %s",
			      get_typec_cc_status_name(pd->cc1),
			      get_typec_cc_status_name(cc1),
			      get_typec_cc_status_name(pd->cc2),
			      get_typec_cc_status_name(cc2));
		pd->cc1 = cc1;
		pd->cc2 = cc2;
		tcpm_cc_change(pd->tcpm_port);
	}

	if (pd->apsd_done && pd->pending_update_usb_data) {
		logbuffer_log(pd->log,
			      "APSD is done now, update pending usb data role");
		ret = update_usb_data_role(pd);
		if (ret < 0)
			goto unlock;
		pd->pending_update_usb_data = false;
	}
unlock:
	kfree(event);
	mutex_unlock(&pd->lock);
}

static int psy_changed(struct notifier_block *nb, unsigned long evt, void *ptr)
{
	struct usbpd *pd;
	struct psy_changed_event *event;

	pd = container_of(nb, struct usbpd, psy_nb);
	if (ptr == pd->wireless_psy)
		logbuffer_log(pd->log, "wireless supply changed");

	if (!((ptr == pd->usb_psy || ptr == pd->wireless_psy)
	    && evt == PSY_EVENT_PROP_CHANGED))
		return 0;

	event = kzalloc(sizeof(*event), GFP_ATOMIC);
	if (!event)
		return -ENOMEM;

	INIT_WORK(&event->work, psy_changed_handler);
	event->pd = pd;
	queue_work(pd->wq, &event->work);

	return 0;
}

static int tcpm_init(struct tcpc_dev *dev)
{
	return 0;
}

static int tcpm_get_vbus(struct tcpc_dev *dev)
{
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	bool vbus_on;
	int ret;

	mutex_lock(&pd->lock);

	vbus_on = pd->vbus_present || pd->vbus_output;
	logbuffer_log(pd->log, "tcpm_get_vbus: %s",
		      vbus_on ? "True" : "False");
	ret = vbus_on ? 1 : 0;

	mutex_unlock(&pd->lock);
	return ret;
}

static int tcpm_set_cc(struct tcpc_dev *dev, enum typec_cc_status cc)
{
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	mutex_lock(&pd->lock);

	switch (cc) {
	case TYPEC_CC_OPEN:
		val.intval = POWER_SUPPLY_TYPEC_PR_NONE;
		break;
	case TYPEC_CC_RD:
		val.intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	case TYPEC_CC_RP_DEF:
		val.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	case TYPEC_CC_RP_1_5:
		val.intval = POWER_SUPPLY_TYPEC_PR_SOURCE_1_5;
		break;
	default:
		logbuffer_log(pd->log, "tcpm_set_cc: invalid cc %s",
			      get_typec_cc_status_name(cc));
		ret = -EINVAL;
		goto unlock;
	}

	if (cc == TYPEC_CC_OPEN) {
		/* Set CC to open and block later changes */
		ret = vote(pd->disable_pr_switch, DISABLE_CC_VOTER, true, 0);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "vote disable_pr_switch fail, ret %d",
				      ret);
		}
	} else {
		/* Make CC be configurable */
		ret = vote(pd->disable_pr_switch, DISABLE_CC_VOTER, false, 0);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "unvote disable_pr_switch fail, ret %d",
				      ret);
		}
	}

	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_POWER_ROLE,
					&val);
	if (ret < 0)
		logbuffer_log(pd->log, "unable to set POWER_ROLE, ret=%d",
			      ret);
	else
		logbuffer_log(pd->log, "set POWER_ROLE to %d",
			      cc);

unlock:
	mutex_unlock(&pd->lock);
	return ret;
}

static int tcpm_get_cc(struct tcpc_dev *dev, enum typec_cc_status *cc1,
		       enum typec_cc_status *cc2)
{
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);

	mutex_lock(&pd->lock);

	logbuffer_log(pd->log, "tcpm_get_cc: cc1=%s, cc2=%s",
		      get_typec_cc_status_name(pd->cc1),
		      get_typec_cc_status_name(pd->cc2));
	*cc1 = pd->cc1;
	*cc2 = pd->cc2;

	mutex_unlock(&pd->lock);
	return 0;
}

static int tcpm_set_polarity(struct tcpc_dev *dev,
			     enum typec_cc_polarity polarity)
{
	/* Hardware handles polarity, no op here. */
	return 0;
}

static int tcpm_set_vconn(struct tcpc_dev *dev, bool on)
{
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	mutex_lock(&pd->lock);

	if (on != pd->vconn_output) {
		if (on)
			ret = regulator_enable(pd->vconn);
		else
			ret = regulator_disable(pd->vconn);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "unable to turn %s vconn, ret=%d",
				      on ? "on" : "off", ret);
			goto unlock;
		}
		pd->vconn_output = on;
	}

	logbuffer_log(pd->log, "set vconn: %s", on ? "on" : "off");

unlock:
	mutex_unlock(&pd->lock);
	return ret;
}

static int tcpm_set_vbus(struct tcpc_dev *dev, bool on, bool charge)
{
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;
	int usb_icl;
	bool work_flushed, apsd_disabled;

	work_flushed = flush_delayed_work(&pd->ext_vbus_work);
	logbuffer_log(pd->log, "Flushed ext vbus delayed work: %s",
		      work_flushed ? "yes" : "no");
	mutex_lock(&pd->lock);

	if (on) {
		/* disable charging */
		ret = vote(pd->usb_icl_votable, OTG_ICL_VOTER, true, 0);
		if (ret < 0) {
			logbuffer_log(pd->log, "vote usb_icl 0 fail, ret %d",
				      ret);
			goto unlock;
		}
		/* disable APSD */
		ret = vote(pd->apsd_disable_votable,
			   OTG_DISABLE_APSD_VOTER, true, 0);
		if (ret < 0) {
			logbuffer_log(pd->log, "vote apsd_disable fail, ret %d",
				      ret);
			goto vote_icl_default;
		}
	} else {
		/* enable APSD */
		ret = vote(pd->apsd_disable_votable,
			   OTG_DISABLE_APSD_VOTER, false, 0);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "unvote apsd_disable fail, ret %d",
				      ret);
			goto vote_icl_default;
		}
		/* enable charging */
		ret = vote(pd->usb_icl_votable, OTG_ICL_VOTER, false, 0);
		if (ret < 0) {
			logbuffer_log(pd->log, "unvote usb_icl fail, ret %d",
				      ret);
			goto unlock;
		}
	}

	apsd_disabled = get_effective_result(pd->apsd_disable_votable);
	usb_icl = get_effective_result(pd->usb_icl_votable);
	if (on && !apsd_disabled) {
		logbuffer_log(pd->log, "apsd not disabled, icl = %d", usb_icl);
		goto vote_apsd_default;
	}

	ret = update_vbus_locked(pd, on);

	if (ret)
		goto vote_apsd_default;

	logbuffer_log(pd->log, "set vbus: %s, apsd: %s, icl: %d",
		      on ? "on" : "off",
		      apsd_disabled ? "disabled" : "enabled",
		      usb_icl);
	goto unlock;

vote_apsd_default:
	/* enable APSD by default */
	ret = vote(pd->apsd_disable_votable,
		   OTG_DISABLE_APSD_VOTER, false, 0);
	if (ret < 0)
		logbuffer_log(pd->log, "unvote apsd_disable fail, ret %d", ret);
vote_icl_default:
	/* enable charging by default */
	ret = vote(pd->usb_icl_votable, OTG_ICL_VOTER, false, 0);
	if (ret < 0)
		logbuffer_log(pd->log, "unvote usb_icl fail, ret %d", ret);
unlock:
	mutex_unlock(&pd->lock);
	return ret;
}

static int tcpm_set_current_limit(struct tcpc_dev *dev, u32 max_ma, u32 min_mv,
				  u32 max_mv)
{
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	val.intval = max_mv * 1000;
	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_VOLTAGE_MAX,
					&val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set max voltage to %d, ret=%d",
			      max_mv, ret);
		return ret;
	}

	val.intval = min_mv * 1000;
	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_VOLTAGE_MIN,
					&val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set min voltage to %d, ret=%d",
			      min_mv, ret);
		return ret;
	}

	/*
	 * smb-lib manages the current limits when pd capable partner is
	 * not attached.
	 */
	if (pd->pd_capable) {
		val.intval = max_ma * 1000;
		ret = power_supply_set_property(pd->usb_psy,
						POWER_SUPPLY_PROP_PD_CURRENT_MAX,
						&val);
		if (ret < 0) {
			logbuffer_log(pd->log,
				      "unable to set pd current max to %d, ret=%d",
				      max_ma, ret);
			return ret;
		}
	}

	logbuffer_log(pd->log,
		      "max_ma := %d, min_mv := %d, max_mv := %d, pd_capable := %c",
		      max_ma, min_mv, max_mv, pd->pd_capable ? 'Y' : 'N');
	return ret;
}

static int tcpm_set_pd_rx(struct tcpc_dev *dev, bool on)
{
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	if (pd->pdphy_open == on) {
		logbuffer_log(pd->log, "pd_phy already %s",
			      on ? "open" : "closed");
		return 0;
	}

	if (on) {
		ret = pd_phy_open(&pd->pdphy_params);
		if (ret < 0) {
			logbuffer_log(pd->log, "unable to %s pd_phy, ret=%d",
				      on ? "open" : "close", ret);
			return ret;
		}
	} else {
		/* pd_phy_close() has no return value. */
		pd_phy_close();
	}

	pd->pdphy_open = on;
	logbuffer_log(pd->log, "%s pd_phy", on ? "open" : "close");

	return 0;
}

static int get_data_len(__le16 header)
{
	int ret;

	ret = pd_header_cnt(__le16_to_cpu(header));
	ret *= 4;

	return ret;
}

static const char * const get_tcpm_transmit_type_name(
		enum tcpm_transmit_type type)
{
	switch (type) {
	case TCPC_TX_SOP:
		return "SOP";
	case TCPC_TX_SOP_PRIME:
		return "SOP_PRIME";
	case TCPC_TX_SOP_PRIME_PRIME:
		return "SOP_PRIME_PRIME";
	case TCPC_TX_SOP_DEBUG_PRIME:
		return "SOP_DEBUG_PRIME";
	case TCPC_TX_SOP_DEBUG_PRIME_PRIME:
		return "SOP_DEBUG_PRIME_PRIME";
	case TCPC_TX_HARD_RESET:
		return "HARD_RESET";
	case TCPC_TX_CABLE_RESET:
		return "CABLE_RESET";
	case TCPC_TX_BIST_MODE_2:
		return "BIST_MODE_2";
	default:
		return "UNDEFINED";
	}
}

struct pd_transmit_work {
	struct work_struct work;
	struct usbpd *pd;
	enum tcpm_transmit_type type;
	struct pd_message msg;
};

static const char * const get_tcpm_transmit_status_name(
		enum tcpm_transmit_status status)
{
	switch (status) {
	case TCPC_TX_SUCCESS:
		return "SUCCESS";
	case TCPC_TX_DISCARDED:
		return "DISCARDED";
	case TCPC_TX_FAILED:
		return "FAILED";
	default:
		return "UNDEFINED";
	}
}

static void pd_transmit_handler(struct work_struct *work)
{
	struct pd_transmit_work *pd_tx_work = container_of(work,
					struct pd_transmit_work,
					work);
	struct usbpd *pd = pd_tx_work->pd;
	enum tcpm_transmit_type type = pd_tx_work->type;
	struct pd_message *msg = &pd_tx_work->msg;
	enum tcpm_transmit_status status;
	bool signal;
	int ret = 0;

	switch (type) {
	case TCPC_TX_HARD_RESET:
		ret = pd_phy_signal(HARD_RESET_SIG);
		signal = true;
		break;
	case TCPC_TX_CABLE_RESET:
		ret = pd_phy_signal(CABLE_RESET_SIG);
		signal = true;
		break;
	case TCPC_TX_SOP:
		ret = pd_phy_write(msg->header, (u8 *)msg->payload,
				   get_data_len(msg->header),
				   SOP_MSG);
		signal = false;
		break;
	case TCPC_TX_SOP_PRIME:
		ret = pd_phy_write(msg->header, (u8 *)msg->payload,
				   get_data_len(msg->header),
				   SOPI_MSG);
		signal = false;
		break;
	case TCPC_TX_SOP_PRIME_PRIME:
		ret = pd_phy_write(msg->header, (u8 *)msg->payload,
				   get_data_len(msg->header),
				   SOPII_MSG);
		signal = false;
		break;
	default:
		logbuffer_log(pd->log, "unknown pd tx type");
		kfree(pd_tx_work);
		return;
	}
	/*
	 * pd_phy_write()/pd_phy_signal() return value for hw irq event:
	 * - tx succeeded hw irq: 0 for signal or actual tx len for write
	 * - tx discarded hw irq: -EBUSY
	 * - tx failed hw irq: -EFAULT
	 */
	if (ret >= 0)
		status = TCPC_TX_SUCCESS;
	else if (ret == -EBUSY)
		status = TCPC_TX_DISCARDED;
	else
		status = TCPC_TX_FAILED;

	tcpm_pd_transmit_complete(pd->tcpm_port, status);

	if (signal)
		logbuffer_log(pd->log,
			      "pd tx type [%s], pdphy ret [%d], status [%s]",
			      get_tcpm_transmit_type_name(type),
			      ret, get_tcpm_transmit_status_name(status));
	else
		logbuffer_log(pd->log,
			      "pd tx header [%#x], type [%s], pdphy ret [%d], status [%s]",
			      msg->header, get_tcpm_transmit_type_name(type),
			      ret, get_tcpm_transmit_status_name(status));

	kfree(pd_tx_work);
}

static int tcpm_pd_transmit(struct tcpc_dev *dev, enum tcpm_transmit_type type,
			    const struct pd_message *msg)
{
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	struct pd_transmit_work *pd_tx_work;

	switch (type) {
	case TCPC_TX_HARD_RESET:
	case TCPC_TX_CABLE_RESET:
	case TCPC_TX_SOP:
	case TCPC_TX_SOP_PRIME:
	case TCPC_TX_SOP_PRIME_PRIME:
		break;
	default:
		logbuffer_log(pd->log, "unsupported pd type: %s",
			      get_tcpm_transmit_type_name(type));
		return -EINVAL;
	}

	pd_tx_work = kzalloc(sizeof(*pd_tx_work), GFP_ATOMIC);
	if (!pd_tx_work)
		return -ENOMEM;
	INIT_WORK(&pd_tx_work->work, pd_transmit_handler);
	pd_tx_work->pd = pd;
	pd_tx_work->type = type;
	if (msg)
		memcpy(&pd_tx_work->msg, msg, sizeof(*msg));
	queue_work(pd->wq, &pd_tx_work->work);

	if (msg)
		logbuffer_log(pd->log, "queue pd tx header [%#x], type [%s]",
			      msg->header, get_tcpm_transmit_type_name(type));
	else
		logbuffer_log(pd->log, "queue pd tx type [%s]",
			      get_tcpm_transmit_type_name(type));
	return 0;
}

static int tcpm_start_drp_toggling(struct tcpc_dev *dev,
				   enum typec_cc_status cc)
{
	/*
	 * Ignore the typec_cc_status for now. As current no
	 * src_pdo is configured, the default is Rp_def.
	 */
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret;

	mutex_lock(&pd->lock);

	/* Make CC be configurable */
	ret = vote(pd->disable_pr_switch, DISABLE_CC_VOTER, false, 0);
	if (ret < 0)
		logbuffer_log(pd->log, "unvote disable_pr_switch fail, ret %d",
			      ret);

	val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;

	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_POWER_ROLE,
					&val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set POWER_ROLE to PR_DUAL, ret=%d",
			      ret);
		goto unlock;
	}

	pd->cc1 = TYPEC_CC_OPEN;
	pd->cc2 = TYPEC_CC_OPEN;

	logbuffer_log(pd->log, "start toggling");

	/* Force a recheck of the CC status since cc lines are
	 * assumed to be open.
	 */
	psy_changed(&pd->psy_nb, PSY_EVENT_PROP_CHANGED, pd->usb_psy);

unlock:
	mutex_unlock(&pd->lock);
	return ret;
}

static int tcpm_set_in_pr_swap(struct tcpc_dev *dev, bool pr_swap)
{
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	mutex_lock(&pd->lock);
	if (pd->in_pr_swap != pr_swap) {
		if (!pr_swap) {
			val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
			ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_POWER_ROLE,
					&val);
			if (ret < 0) {
				logbuffer_log(pd->log,
				"unable to set POWER_ROLE to PR_DUAL, ret=%d",
					ret);
				goto unlock;
			}
			/* Required for the PMIC to recover */
			logbuffer_log(pd->log, "sleeping for 20mec");
			msleep(20);
			psy_changed(&pd->psy_nb, PSY_EVENT_PROP_CHANGED,
				    pd->usb_psy);
		}
		val.intval = pr_swap ? 1 : 0;
		ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PR_SWAP,
					&val);
		if (ret < 0) {
			logbuffer_log(pd->log,
					"unable to set PR_SWAP to %d, ret=%d",
					pr_swap ? 1 : 0, ret);
			goto unlock;
		}

		pd->in_pr_swap = pr_swap;
		logbuffer_log(pd->log, "PR_SWAP = %d", pr_swap ? 1 : 0);
	}
unlock:
	mutex_unlock(&pd->lock);
	return ret;
}

static int tcpm_in_pd_contract(struct tcpc_dev *dev,
			       bool status)
{
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	mutex_lock(&pd->lock);

	if (status == pd->in_explicit_contract)
		goto unlock;

	/* Attempt once */
	val.intval = status ? 1 : 0;
	logbuffer_log(pd->log, "pd contract %d", status ? 1 : 0);
	ret = power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_EXPLICIT_CONTRACT,
				&val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set pd contract to %d, ret=%d",
			      status ? 1 : 0, ret);
	} else {
		pd->in_explicit_contract = status;
	}

unlock:
	mutex_unlock(&pd->lock);
	return ret;
}

static int tcpm_set_suspend_supported(struct tcpc_dev *dev,
				      bool suspend_supported)
{
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	mutex_lock(&pd->lock);

	if (suspend_supported == pd->suspend_supported)
		goto unlock;

	/* Attempt once */
	pd->suspend_supported = suspend_supported;
	val.intval = suspend_supported ? 1 : 0;
	logbuffer_log(pd->log, "usb suspend %d", suspend_supported ? 1 : 0);
	ret = power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED,
				&val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set suspend flag to %d, ret=%d",
			      suspend_supported ? 1 : 0, ret);
	}

unlock:
	mutex_unlock(&pd->lock);
	return ret;
}

enum power_role get_pdphy_power_role(enum typec_role role)
{
	switch (role) {
	case TYPEC_SINK:
		return PR_SINK;
	case TYPEC_SOURCE:
		return PR_SRC;
	default:
		return PR_NONE;
	}
}

static const char * const get_typec_role_name(enum typec_role role)
{
	switch (role) {
	case TYPEC_SINK:
		return "SINK";
	case TYPEC_SOURCE:
		return "SOURCE";
	default:
		return "UNDEFINED";
	}
}

static const char * const get_pdphy_pr_name(enum power_role pdphy_pr)
{
	switch (pdphy_pr) {
	case PR_SINK:
		return "PR_SINK";
	case PR_SRC:
		return "PR_SRC";
	case PR_NONE:
		return "PR_NONE";
	default:
		return "UNDEFINED";
	}
}

enum data_role get_pdphy_data_role(enum typec_data_role data)
{
	switch (data) {
	case TYPEC_DEVICE:
		return DR_UFP;
	case TYPEC_HOST:
		return DR_DFP;
	default:
		return DR_NONE;
	}
}

static const char * const get_typec_data_role_name(enum typec_data_role data)
{
	switch (data) {
	case TYPEC_DEVICE:
		return "DEVICE";
	case TYPEC_HOST:
		return "HOST";
	default:
		return "UNDEFINED";
	}
}

static const char * const get_pdphy_dr_name(enum data_role pdphy_dr)
{
	switch (pdphy_dr) {
	case DR_UFP:
		return "DR_UFP";
	case DR_DFP:
		return "DR_DFP";
	case DR_NONE:
		return "DR_NONE";
	default:
		return "UNDEFINED";
	}
}

static int set_pd_header(struct usbpd *pd, enum typec_role role,
			 enum typec_data_role data)
{
	enum power_role pdphy_pr;
	enum data_role pdphy_dr;
	int ret = 0;

	pdphy_pr = get_pdphy_power_role(role);
	pdphy_dr = get_pdphy_data_role(data);
	ret = pd_phy_update_roles(pdphy_dr, pdphy_pr);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set pd_phy_header: %s, %s, ret=%d",
			      get_pdphy_pr_name(pdphy_pr),
			      get_pdphy_dr_name(pdphy_dr),
			      ret);
		return ret;
	}

	logbuffer_log(pd->log, "set pd_phy_header: %s, %s",
		      get_pdphy_pr_name(pdphy_pr),
		      get_pdphy_dr_name(pdphy_dr));

	return 0;
}

#define EXTCON_USB_SUPER_SPEED	true
static int set_usb_data_role(struct usbpd *pd, bool attached,
			     enum typec_role role,
			     enum typec_data_role data,
			     bool usb_comm_capable)
{
	int ret = 0;

	pd->extcon_usb_cc = pd->is_cable_flipped;

	if (!attached) {
		pd->extcon_usb = false;
		pd->extcon_usb_host = false;
	} else if (data == TYPEC_HOST) {
		pd->extcon_usb = false;
		pd->extcon_usb_host = true;
	} else {
		pd->extcon_usb = true;
		pd->extcon_usb_host = false;
	}

	pd->usb_comm_capable = usb_comm_capable;

	logbuffer_log(pd->log,
		      "set usb_data_role: power [%s], data [%s], apsd_done [%s], attached [%s], comm [%s]",
		      get_typec_role_name(role),
		      get_typec_data_role_name(data),
		      pd->apsd_done ? "Y" : "N",
		      attached ? "Y" : "N",
		      usb_comm_capable ? "Y" : "N");

	if (attached && role == TYPEC_SINK &&
	    !(pd->apsd_done || usb_comm_capable)) {
		/* wait for APSD done */
		logbuffer_log(pd->log,
			      "APSD is not done, delay update usb data role");
		pd->pending_update_usb_data = true;
	} else {
		ret = update_usb_data_role(pd);
		if (ret < 0)
			return ret;
		pd->pending_update_usb_data = false;
	}

	return ret;
}

static int tcpm_set_roles(struct tcpc_dev *dev, bool attached,
			  enum typec_role role, enum typec_data_role data,
			  bool usb_comm_capable)
{
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	mutex_lock(&pd->lock);

	ret = set_pd_header(pd, role, data);
	if (ret < 0)
		goto unlock;

	ret = set_usb_data_role(pd, attached, role, data, usb_comm_capable);

	if (!ret) {
		pd->cur_pwr_role = role;
		pd->cur_data_role = data;
	}
unlock:
	mutex_unlock(&pd->lock);
	return ret;
}

static void pd_phy_signal_rx(struct usbpd *pd, enum pd_sig_type type)
{
	switch (type) {
	case HARD_RESET_SIG:
		tcpm_pd_hard_reset(pd->tcpm_port);
		logbuffer_log(pd->log, "received pd hard reset");
		break;
	default:
		logbuffer_log(pd->log, "received unsupported signal: %d",
			      type);
		return;
	}
}

static void pd_phy_message_rx(struct usbpd *pd, enum pd_sop_type sop,
			      u8 *buf, size_t len)
{
	struct pd_message msg;

	if (len < 2) {
		logbuffer_log(pd->log, "invalid message received, len=%ld",
			      len);
		return;
	}

	if (sop != SOP_MSG) {
		logbuffer_log(pd->log,
			      "invalid msg type (%d) received; only SOP supported\n",
			      sop);
		return;
	}

	msg.header = cpu_to_le16(*((u16 *)buf));
	buf += sizeof(u16);
	len -= sizeof(u16);
	if (get_data_len(msg.header) != len) {
		logbuffer_log(pd->log, "header len %d != len %ld",
			      get_data_len(msg.header), len);
		return;
	}
	if (len > PD_MAX_PAYLOAD * 4) {
		logbuffer_log(pd->log, "len %ld > PD_MAX_PAYLOAD", len);
		return;
	}
	memcpy(msg.payload, buf, len);
	logbuffer_log(pd->log, "pd rx header [%#x]", msg.header);
	tcpm_pd_receive(pd->tcpm_port, &msg);
}

static void pd_phy_shutdown(struct usbpd *pd)
{
	int rc = 0;

	flush_delayed_work(&pd->ext_vbus_work);
	mutex_lock(&pd->lock);
	if (regulator_is_enabled(pd->vbus)) {
		rc = regulator_disable(pd->vbus);
		if (rc < 0) {
			pr_err("unable to disable vbus\n");
		} else {
			pd->vbus_output = false;
			pd->smb2_vbus_reg = false;
		}
	}
	if (pd->ext_vbus_reg) {
		rc = regulator_disable(pd->ext_vbus);
		if (rc < 0) {
			pr_err("unable to disable vbus\n");
		} else {
			pd->vbus_output = false;
			pd->ext_vbus_reg = false;
		}
	}
	if (regulator_is_enabled(pd->vconn)) {
		rc = regulator_disable(pd->vconn);
		if (rc < 0)
			pr_err("unable to disable vconn\n");
		else
			pd->vconn_output = false;
	}
	mutex_unlock(&pd->lock);

	logbuffer_log(pd->log, "pd phy shutdown");
}

static void log_rtc(struct tcpc_dev *dev)
{
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);

	logbuffer_log(pd->log, NULL);

	return;
}

static void set_pd_capable(struct tcpc_dev *dev, bool capable)
{
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	val.intval = capable ? 1 : 0;

	if (pd->pd_capable == capable)
		return;

	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_ACTIVE,
					&val);
	if (ret < 0) {
		logbuffer_log(pd->log, "unable to set pd capable to %s, ret=%d",
			      capable ? "true" : "false", ret);
	}

	pd->pd_capable = capable;
}

static void set_in_hard_reset(struct tcpc_dev *dev, bool status)
{
	union power_supply_propval val = {0};
	struct usbpd *pd = container_of(dev, struct usbpd, tcpc_dev);
	int ret = 0;

	mutex_lock(&pd->lock);

	if (status == pd->in_hard_reset)
		goto unlock;

	val.intval = status ? 1 : 0;
	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_IN_HARD_RESET,
					&val);
	if (ret < 0) {
		logbuffer_log(pd->log,
			      "unable to set hard reset status to %s, ret=%d",
			      status ? "true" : "false", ret);
		goto unlock;
	}

	switch (pd->cur_pwr_role) {
	case TYPEC_SINK:
		val.intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	case TYPEC_SOURCE:
		val.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	default:
		goto unlock;
	}
	ret = power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_POWER_ROLE,
					&val);
	if (ret < 0)
		logbuffer_log(pd->log, "unable to set POWER_ROLE, ret=%d",
			      ret);

	pd->in_hard_reset = status;

unlock:
	mutex_unlock(&pd->lock);
}

enum pdo_role {
	SNK_PDO,
	SRC_PDO,
};

static const char * const pdo_prop_name[] = {
	[SNK_PDO]	= "goog,snk-pdo",
	[SRC_PDO]	= "goog,src-pdo",
};

static u32 *parse_pdo(struct usbpd *pd, enum pdo_role role,
		      unsigned int *nr_pdo)
{
	struct device *dev = &pd->dev;
	u32 *dt_array;
	u32 *pdo;
	int i, count, rc;

	count = device_property_read_u32_array(dev->parent, pdo_prop_name[role],
					       NULL, 0);
	if (count > 0) {
		if (count % 4)
			return ERR_PTR(-EINVAL);

		*nr_pdo = count / 4;
		dt_array = devm_kcalloc(dev, count, sizeof(*dt_array),
					GFP_KERNEL);
		if (!dt_array)
			return ERR_PTR(-ENOMEM);

		rc = device_property_read_u32_array(dev->parent,
						    pdo_prop_name[role],
						    dt_array, count);
		if (rc)
			return ERR_PTR(rc);

		pdo = devm_kcalloc(dev, *nr_pdo, sizeof(*pdo), GFP_KERNEL);
		if (!pdo)
			return ERR_PTR(-ENOMEM);

		for (i = 0; i < *nr_pdo; i++) {
			switch (dt_array[i * 4]) {
			case PDO_TYPE_FIXED:
				pdo[i] = PDO_FIXED(dt_array[i * 4 + 1],
						   dt_array[i * 4 + 2],
						   PDO_FIXED_FLAGS);
				break;
			case PDO_TYPE_BATT:
				pdo[i] = PDO_BATT(dt_array[i * 4 + 1],
						  dt_array[i * 4 + 2],
						  dt_array[i * 4 + 3]);
				break;
			case PDO_TYPE_VAR:
				pdo[i] = PDO_VAR(dt_array[i * 4 + 1],
						 dt_array[i * 4 + 2],
						 dt_array[i * 4 + 3]);
				break;
			/*case PDO_TYPE_AUG:*/
			default:
				return ERR_PTR(-EINVAL);
			}
		}
		return pdo;
	}

	return ERR_PTR(-EINVAL);
}

static int init_tcpc_config(struct tcpc_dev *pd_tcpc_dev)
{
	struct usbpd *pd = container_of(pd_tcpc_dev, struct usbpd, tcpc_dev);
	struct device *dev = &pd->dev;
	struct tcpc_config *config;
	int ret;

	pd_tcpc_dev->config = devm_kzalloc(dev, sizeof(*config), GFP_KERNEL);
	if (!pd_tcpc_dev->config)
		return -ENOMEM;

	config = pd_tcpc_dev->config;

	ret = device_property_read_u32(dev->parent, "goog,port-type",
				       &config->type);
	if (ret < 0)
		return ret;

	switch (config->type) {
	case TYPEC_PORT_UFP:
		config->snk_pdo = parse_pdo(pd, SNK_PDO, &config->nr_snk_pdo);
		if (IS_ERR(config->snk_pdo))
			return PTR_ERR(config->snk_pdo);
		break;
	case TYPEC_PORT_DFP:
		config->src_pdo = parse_pdo(pd, SRC_PDO, &config->nr_src_pdo);
		if (IS_ERR(config->src_pdo))
			return PTR_ERR(config->src_pdo);
		break;
	case TYPEC_PORT_DRP:
		config->snk_pdo = parse_pdo(pd, SNK_PDO, &config->nr_snk_pdo);
		if (IS_ERR(config->snk_pdo))
			return PTR_ERR(config->snk_pdo);
		config->src_pdo = parse_pdo(pd, SRC_PDO, &config->nr_src_pdo);
		if (IS_ERR(config->src_pdo))
			return PTR_ERR(config->src_pdo);

		ret = device_property_read_u32(dev->parent, "goog,default-role",
					       &config->default_role);
		if (ret < 0)
			return ret;

		config->try_role_hw = device_property_read_bool(
							dev->parent,
							"goog,try-role-hw");
		break;
	default:
		return -EINVAL;
	}

	if (config->type == TYPEC_PORT_UFP || config->type == TYPEC_PORT_DRP) {
		ret = device_property_read_u32(dev->parent, "goog,max-snk-mv",
					       &config->max_snk_mv);
		ret = device_property_read_u32(dev->parent, "goog,max-snk-ma",
					       &config->max_snk_ma);
		ret = device_property_read_u32(dev->parent, "goog,max-snk-mw",
					       &config->max_snk_mw);
		ret = device_property_read_u32(dev->parent, "goog,op-snk-mw",
					       &config->operating_snk_mw);
		if (ret < 0)
			return ret;
	}

	/* TODO: parse alt mode from DT */
	config->alt_modes = NULL;
	config->self_powered = true;

	return 0;
}

static int init_tcpc_dev(struct tcpc_dev *pd_tcpc_dev)
{
	int ret;

	ret = init_tcpc_config(pd_tcpc_dev);
	if (ret < 0)
		return ret;
	pd_tcpc_dev->init = tcpm_init;
	pd_tcpc_dev->get_vbus = tcpm_get_vbus;
	pd_tcpc_dev->set_cc = tcpm_set_cc;
	pd_tcpc_dev->get_cc = tcpm_get_cc;
	pd_tcpc_dev->set_polarity = tcpm_set_polarity;
	pd_tcpc_dev->set_vconn = tcpm_set_vconn;
	pd_tcpc_dev->set_vbus = tcpm_set_vbus;
	pd_tcpc_dev->set_current_limit = tcpm_set_current_limit;
	pd_tcpc_dev->set_pd_rx = tcpm_set_pd_rx;
	pd_tcpc_dev->set_roles = tcpm_set_roles;
	pd_tcpc_dev->try_role = NULL;
	pd_tcpc_dev->pd_transmit = tcpm_pd_transmit;
	pd_tcpc_dev->start_drp_toggling = tcpm_start_drp_toggling;
	pd_tcpc_dev->set_in_pr_swap = tcpm_set_in_pr_swap;
	pd_tcpc_dev->set_pd_capable = set_pd_capable;
	pd_tcpc_dev->set_in_hard_reset = set_in_hard_reset;
	pd_tcpc_dev->log_rtc = log_rtc;
	pd_tcpc_dev->set_suspend_supported = tcpm_set_suspend_supported;
	pd_tcpc_dev->in_pd_contract = tcpm_in_pd_contract;
	pd_tcpc_dev->mux = NULL;
	return 0;
}

static void init_pd_phy_params(struct pd_phy_params *pdphy_params)
{
	pdphy_params->signal_cb = pd_phy_signal_rx;
	pdphy_params->msg_rx_cb = pd_phy_message_rx;
	pdphy_params->shutdown_cb = pd_phy_shutdown;
	pdphy_params->data_role = DR_UFP;
	pdphy_params->power_role = PR_SINK;
	pdphy_params->frame_filter_val = FRAME_FILTER_EN_SOP |
					 FRAME_FILTER_EN_HARD_RESET;
}

static int update_ext_vbus(struct notifier_block *self, unsigned long action,
			   void *dev)
{
	struct usbpd *pd = container_of(self, struct usbpd, ext_vbus_nb);
	bool turn_on_ext_vbus = (action == EXT_VBUS_ON) ? true : false;
	bool work_queued, work_cancelled;

	work_cancelled = cancel_delayed_work_sync(&pd->ext_vbus_work);
	logbuffer_log(pd->log, "ext_vbus_work_cancelled: %s",
		      work_cancelled ? "yes" : "no");
	if (work_cancelled)
		pm_relax(&pd->dev);

	mutex_lock(&pd->lock);
	pd->external_vbus_update = turn_on_ext_vbus;
	work_queued = queue_delayed_work(pd->wq, &pd->ext_vbus_work,
				turn_on_ext_vbus ?
				msecs_to_jiffies(EXT_VBUS_WORK_DELAY_MS)
				: 0);
	if (!work_queued)
		logbuffer_log(pd->log, "error: queueing ext_vbus_work failed");
	else {
		pm_stay_awake(&pd->dev);
		logbuffer_log(pd->log, "queued work EXT_VBUS_%s",
			      (action == EXT_VBUS_ON) ?
			      "ON" : "OFF");
	}
	mutex_unlock(&pd->lock);
	return NOTIFY_OK;
}

static const unsigned int usbpd_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

/* EXTCON_USB and EXTCON_USB_HOST are mutually exclusive */
static const u32 usbpd_extcon_exclusive[] = {0x3, 0};

static void usbpd_release(struct device *dev)
{
	struct usbpd *pd = container_of(dev, struct usbpd, dev);

	kfree(pd);
}

static int num_pd_instances;
/**
 * usbpd_create - Create a new instance of USB PD protocol/policy engine
 * @parent - parent device to associate with
 *
 * This creates a new usbpd class device which manages the state of a
 * USB PD-capable port. The parent device that is passed in should be
 * associated with the physical device port, e.g. a PD PHY.
 *
 * Derived from policy_engine.c.
 *
 * Return: struct usbpd pointer, or an ERR_PTR value
 */
struct usbpd *usbpd_create(struct device *parent)
{
	int ret = 0;
	struct usbpd *pd;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	pd->log = debugfs_logbuffer_register("usbpd");

	if (IS_ERR_OR_NULL(pd->log)) {
		ret = PTR_ERR(pd->log);
		goto free_pd;
	}

	mutex_init(&pd->lock);

	device_initialize(&pd->dev);
	pd->dev.parent = parent;
	pd->dev.release = usbpd_release;
	dev_set_drvdata(&pd->dev, pd);

	ret = dev_set_name(&pd->dev, "usbpd%d", num_pd_instances++);
	if (ret < 0)
		goto free_buffer;

	ret = device_add(&pd->dev);
	if (ret < 0)
		goto free_buffer;

	pd->wlc_supported = device_property_read_bool(parent,
						      "goog,wlc-supported");

	ret = pd_engine_debugfs_init(pd);
	if (ret < 0)
		goto del_pd;

	device_init_wakeup(&pd->dev, true);

	pd->vbus = devm_regulator_get(parent, "vbus");
	if (IS_ERR(pd->vbus)) {
		ret = PTR_ERR(pd->vbus);
		goto exit_debugfs;
	}

	pd->vconn = devm_regulator_get(parent, "vconn");
	if (IS_ERR(pd->vconn)) {
		ret = PTR_ERR(pd->vconn);
		goto exit_debugfs;
	}

	pd->ext_vbus = devm_regulator_get(parent, "ext-vbus");
	if (IS_ERR(pd->ext_vbus)) {
		ret = PTR_ERR(pd->ext_vbus);
		goto exit_debugfs;
	}

	pd->extcon = devm_extcon_dev_allocate(parent, usbpd_extcon_cable);
	if (IS_ERR(pd->extcon)) {
		logbuffer_log(pd->log, "failed to allocate extcon device");
		ret = PTR_ERR(pd->extcon);
		goto exit_debugfs;
	}

	pd->extcon->mutually_exclusive = usbpd_extcon_exclusive;
	ret = devm_extcon_dev_register(parent, pd->extcon);
	if (ret < 0) {
		logbuffer_log(pd->log, "failed to register extcon device");
		goto exit_debugfs;
	}

	/* Support reporting polarity and speed via properties */
	extcon_set_property_capability(pd->extcon, EXTCON_USB,
				       EXTCON_PROP_USB_TYPEC_POLARITY);
	extcon_set_property_capability(pd->extcon, EXTCON_USB,
				       EXTCON_PROP_USB_SS);
	extcon_set_property_capability(pd->extcon, EXTCON_USB_HOST,
				       EXTCON_PROP_USB_TYPEC_POLARITY);
	extcon_set_property_capability(pd->extcon, EXTCON_USB_HOST,
				       EXTCON_PROP_USB_SS);

	pd->wq = create_singlethread_workqueue(dev_name(&pd->dev));
	if (!pd->wq) {
		ret = -ENOMEM;
		goto exit_debugfs;
	}

	pd->default_src_cap = true;

	INIT_DELAYED_WORK(&pd->ext_vbus_work, update_external_vbus);
	INIT_WORK(&pd->update_pdo_work, update_src_caps);

	pd->usb_psy = power_supply_get_by_name("usb");
	if (!pd->usb_psy) {
		logbuffer_log(pd->log,
			      "Could not get USB power_supply, deferring probe");
		ret = -EPROBE_DEFER;
		goto del_wq;
	}

	if (pd->wlc_supported) {
		pd->wireless_psy = power_supply_get_by_name("wireless");
		if (!pd->wireless_psy) {
			logbuffer_log(pd->log,
				      "Could not get wireless power_supply, deferring probe");
			ret = -EPROBE_DEFER;
			goto put_psy_usb;
		}
	}

	pd->usb_icl_votable = find_votable("USB_ICL");
	if (pd->usb_icl_votable == NULL) {
		logbuffer_log(pd->log,
			      "Couldn't find USB_ICL votable, deferring probe");
		ret = -EPROBE_DEFER;
		goto put_psy_wireless;
	}

	pd->apsd_disable_votable = find_votable("APSD_DISABLE");
	if (pd->apsd_disable_votable == NULL) {
		logbuffer_log(pd->log,
			      "Couldn't find APSD_DISABLE votable, deferring probe");
		ret = -EPROBE_DEFER;
		goto put_psy_wireless;
	}

	pd->disable_pr_switch = find_votable("DISABLE_POWER_ROLE_SWITCH");
	if (pd->disable_pr_switch == NULL) {
		logbuffer_log(pd->log,
			      "Couldn't find DISABLE_POWER_ROLE_SWITCH votable, deferring");
		ret = -EPROBE_DEFER;
		goto put_psy_wireless;
	}

	/* initialize votable */
	vote(pd->usb_icl_votable, OTG_ICL_VOTER, false, 0);
	vote(pd->apsd_disable_votable, OTG_DISABLE_APSD_VOTER, false, 0);
	vote(pd->disable_pr_switch, DISABLE_CC_VOTER, false, 0);

	pd->ext_vbus_nb.notifier_call = update_ext_vbus;
	ext_vbus_register_notify(&pd->ext_vbus_nb);

	/*
	 * TCPM callbacks may access pd->usb_psy. Therefore, tcpm_register_port
	 * must be called after pd->usb_psy is initialized.
	 */
	ret = init_tcpc_dev(&pd->tcpc_dev);
	if (ret < 0)
		goto put_psy_wireless;
	pd->tcpm_port = tcpm_register_port(&pd->dev, &pd->tcpc_dev);
	if (IS_ERR(pd->tcpm_port)) {
		ret = PTR_ERR(pd->tcpm_port);
		goto put_psy_wireless;
	}

	psy_changed(&pd->psy_nb, PSY_EVENT_PROP_CHANGED, pd->usb_psy);

	pd->psy_nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&pd->psy_nb);
	if (ret < 0)
		goto unreg_tcpm;

	init_pd_phy_params(&pd->pdphy_params);
	pd_phy_assign_pm_callbacks(&pd->pdphy_params);

	pd->suspend_supported = true;

	return pd;

unreg_tcpm:
	tcpm_unregister_port(pd->tcpm_port);
put_psy_wireless:
	ext_vbus_unregister_notify(&pd->ext_vbus_nb);
	if (pd->wlc_supported)
		power_supply_put(pd->wireless_psy);
put_psy_usb:
	power_supply_put(pd->usb_psy);
del_wq:
	destroy_workqueue(pd->wq);
exit_debugfs:
	pd_engine_debugfs_exit(pd);
del_pd:
	device_del(&pd->dev);
free_buffer:
	debugfs_logbuffer_unregister(pd->log);
free_pd:
	num_pd_instances--;
	put_device(&pd->dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(usbpd_create);

/**
 * usbpd_destroy - Removes and frees a usbpd instance
 * @pd: the instance to destroy
 *
 * Derived from policy_engine.c.
 */
void usbpd_destroy(struct usbpd *pd)
{
	if (!pd)
		return;
	power_supply_unreg_notifier(&pd->psy_nb);
	tcpm_unregister_port(pd->tcpm_port);
	ext_vbus_unregister_notify(&pd->ext_vbus_nb);
	if (pd->wlc_supported)
		power_supply_put(pd->wireless_psy);
	power_supply_put(pd->usb_psy);
	destroy_workqueue(pd->wq);
	pd_engine_debugfs_exit(pd);
	num_pd_instances--;
	debugfs_logbuffer_unregister(pd->log);
	device_unregister(&pd->dev);
}
EXPORT_SYMBOL(usbpd_destroy);

static int __init pd_engine_init(void)
{
	return 0;
}
module_init(pd_engine_init);

static void __exit pd_engine_exit(void) {}
module_exit(pd_engine_exit);

MODULE_DESCRIPTION("USB PD Engine based on Type-C Port Manager");
MODULE_LICENSE("GPL v2");
