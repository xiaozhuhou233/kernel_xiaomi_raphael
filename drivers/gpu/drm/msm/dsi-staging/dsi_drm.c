/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */


#define pr_fmt(fmt)	"dsi-drm:[%s] " fmt, __func__
#include <linux/msm_drm_notify.h>
#include <linux/clk.h>
#include <linux/ratelimit.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_vblank.h>

#include "msm_kms.h"
#include "sde_connector.h"
#include "dsi_drm.h"
#include "sde_trace.h"
#include "sde_encoder.h"

#define to_dsi_bridge(x)     container_of((x), struct dsi_bridge, base)
#define to_dsi_state(x)      container_of((x), struct dsi_connector_state, base)

#define DEFAULT_PANEL_JITTER_NUMERATOR		2
#define DEFAULT_PANEL_JITTER_DENOMINATOR	1
#define DEFAULT_PANEL_JITTER_ARRAY_SIZE		2
#define DEFAULT_PANEL_PREFILL_LINES	25
#define STEP_REFRESH_VBLANK_COUNT	2
#define STEP_REFRESH_LOCK_RETRIES	3
#define STEP_REFRESH_READY_RETRIES	8
#define STEP_REFRESH_RETRY_DELAY_MS	50
#define STEP_REFRESH_QUEUE_DELAY_MS	1
#define STEP_REFRESH_RESTART_DELAY_MS	30

static struct dsi_display_mode_priv_info default_priv_info = {
	.panel_jitter_numer = DEFAULT_PANEL_JITTER_NUMERATOR,
	.panel_jitter_denom = DEFAULT_PANEL_JITTER_DENOMINATOR,
	.panel_prefill_lines = DEFAULT_PANEL_PREFILL_LINES,
	.dsc_enabled = false,
};

static u32 dsi_bridge_mode_vrefresh(const struct drm_display_mode *mode)
{
	if (!mode)
		return 0;

	return mode->vrefresh ? mode->vrefresh : drm_mode_vrefresh(mode);
}

static struct dsi_display_mode *dsi_bridge_find_refresh_mode(
		struct dsi_display *display,
		const struct dsi_display_mode *reference, u32 refresh_rate)
{
	struct dsi_display_mode *mode, *match = NULL;
	u32 i, count;

	if (!display || !display->panel || !display->modes || !reference)
		return NULL;

	mutex_lock(&display->display_lock);
	count = display->panel->num_display_modes;
	for (i = 0; i < count; i++) {
		mode = &display->modes[i];
		if (mode->timing.refresh_rate != refresh_rate ||
		    mode->timing.h_active != reference->timing.h_active ||
		    mode->timing.v_active != reference->timing.v_active ||
		    mode->panel_mode != reference->panel_mode ||
		    !mode->priv_info || !reference->priv_info ||
		    mode->priv_info->topology.num_lm !=
			reference->priv_info->topology.num_lm ||
		    mode->priv_info->topology.num_enc !=
			reference->priv_info->topology.num_enc ||
		    mode->priv_info->topology.num_intf !=
			reference->priv_info->topology.num_intf)
			continue;

		match = mode;
		break;
	}
	mutex_unlock(&display->display_lock);

	return match;
}

static bool dsi_bridge_step_refresh_host_ready(struct dsi_display *display)
{
	bool host_initialized;
	int rc;
	int i;

	if (!display)
		return false;

	display_for_each_ctrl(i, display) {
		if (!display->ctrl[i].ctrl)
			return false;

		host_initialized = false;
		rc = dsi_ctrl_get_host_engine_init_state(
				display->ctrl[i].ctrl, &host_initialized);
		if (rc || !host_initialized)
			return false;
	}

	return true;
}

static bool dsi_bridge_step_refresh_panel_in_lp(struct dsi_panel *panel)
{
	return panel &&
		(panel->power_mode == SDE_MODE_DPMS_LP1 ||
		 panel->power_mode == SDE_MODE_DPMS_LP2);
}

static u32 dsi_bridge_step_refresh_active_flags(struct dsi_bridge *bridge)
{
	struct drm_crtc *crtc;

	if (!bridge || !bridge->base.encoder)
		return 0;

	crtc = bridge->base.encoder->crtc;
	if (!crtc || !crtc->state)
		return 0;

	return crtc->state->adjusted_mode.private_flags &
		DSI_MODE_FLAG_STEP_REFRESH_ACTIVE;
}

static void dsi_bridge_step_refresh_latch_error(struct dsi_bridge *bridge,
		int rc, const char *action)
{
	struct drm_crtc *crtc;
	u32 flags;
	u32 rate = 0;

	if (!bridge || !rc)
		return;

	flags = dsi_bridge_step_refresh_active_flags(bridge);
	if (!flags)
		return;

	crtc = bridge->base.encoder->crtc;
	if (crtc && crtc->state)
		rate = dsi_bridge_mode_vrefresh(&crtc->state->adjusted_mode);

	atomic_set(&bridge->step_refresh_blocked, 1);
	if (atomic_cmpxchg(&bridge->step_refresh_stage_error, 0, rc) == 0)
		pr_err("[step90] stage error latched during %s at %u Hz flags=0x%x rc=%d\n",
			action, rate, flags, rc);
}

static bool dsi_bridge_step_refresh_generation_current(
	struct dsi_bridge *bridge, u32 generation)
{
	unsigned long irq_flags;
	bool is_current;

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	is_current = !bridge->step_refresh_shutdown &&
		bridge->step_refresh_generation == generation;
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);

	return is_current;
}

static bool dsi_bridge_step_refresh_snapshot(struct dsi_bridge *bridge,
		u32 *flags, u32 *rate, u32 *generation, u32 *retry_count,
		bool *restart)
{
	unsigned long irq_flags;
	bool valid;

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	valid = !bridge->step_refresh_shutdown;
	if (flags)
		*flags = bridge->step_refresh_expected_flags;
	if (rate)
		*rate = bridge->step_refresh_expected_rate;
	if (generation)
		*generation = bridge->step_refresh_generation;
	if (retry_count)
		*retry_count = bridge->step_refresh_retry_count;
	if (restart)
		*restart = atomic_read(&bridge->step_refresh_restart_pending);
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);

	return valid;
}

void dsi_bridge_invalidate_step_refresh(struct dsi_bridge *bridge)
{
	unsigned long irq_flags;

	if (!bridge)
		return;

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	if (!bridge->step_refresh_shutdown) {
		bridge->step_refresh_expected_flags = 0;
		bridge->step_refresh_expected_rate = 0;
		bridge->step_refresh_retry_count = 0;
		bridge->step_refresh_generation++;
		atomic_set(&bridge->step_refresh_restart_pending, 0);
		atomic_set(&bridge->step_refresh_stage_error, 0);
	}
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);
}

void dsi_bridge_request_step_refresh_restart(struct dsi_bridge *bridge)
{
	struct dsi_display *display;
	struct drm_display_mode *adjusted_mode;
	struct drm_display_mode *requested_mode;
	struct drm_crtc *crtc;
	unsigned long irq_flags;
	u32 current_flags;
	u32 current_rate;
	u32 generation = 0;
	u32 requested_rate;
	bool queued = false;

	if (!bridge || !bridge->display || !bridge->display->panel ||
	    !bridge->display->panel->step_refresh_enabled ||
	    !bridge->base.encoder)
		return;

	display = bridge->display;
	crtc = bridge->base.encoder->crtc;
	if (!crtc || !crtc->state || !crtc->state->active)
		return;

	adjusted_mode = &crtc->state->adjusted_mode;
	requested_mode = &crtc->state->mode;
	current_flags = adjusted_mode->private_flags &
			DSI_MODE_FLAG_STEP_REFRESH_ACTIVE;
	current_rate = dsi_bridge_mode_vrefresh(adjusted_mode);
	requested_rate = dsi_bridge_mode_vrefresh(requested_mode);

	if (requested_rate != display->panel->step_refresh_target_rate ||
	    !current_rate)
		return;

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	if (!bridge->step_refresh_shutdown &&
	    !atomic_read(&bridge->step_refresh_restart_pending)) {
		bridge->step_refresh_expected_flags = current_flags;
		bridge->step_refresh_expected_rate = current_rate;
		bridge->step_refresh_retry_count = 0;
		bridge->step_refresh_generation++;
		generation = bridge->step_refresh_generation;
		atomic_set(&bridge->step_refresh_blocked, 0);
		atomic_set(&bridge->step_refresh_restart_pending, 1);
		atomic_set(&bridge->step_refresh_stage_error, 0);
		mod_delayed_work(system_unbound_wq, &bridge->step_refresh_work,
			msecs_to_jiffies(STEP_REFRESH_RESTART_DELAY_MS));
		queued = true;
	}
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);

	if (queued)
		pr_info("[step90] queued LP resume restart at %u Hz generation=%u\n",
			current_rate, generation);
}

static int dsi_bridge_step_refresh_check_ready(struct dsi_bridge *bridge,
		struct drm_crtc *crtc, u32 expected_rate,
		bool allow_clock_pending)
{
	struct dsi_display *display = bridge->display;
	bool host_ready;

	if (!display || !display->panel || !crtc)
		return -ENODEV;

	if (!crtc->state || !crtc->state->active ||
	    dsi_bridge_step_refresh_panel_in_lp(display->panel))
		return -ECANCELED;

	if (atomic_read(&display->panel->esd_recovery_pending))
		return -EIO;

	host_ready = dsi_bridge_step_refresh_host_ready(display);
	if (!display->panel->panel_initialized || !host_ready ||
	    (!allow_clock_pending &&
	     atomic_read(&display->clkrate_change_pending))) {
		pr_info("[step90] %u Hz not ready: panel=%d power=%d host=%d clk_pending=%d\n",
			expected_rate, display->panel->panel_initialized,
			display->panel->power_mode,
			host_ready,
			atomic_read(&display->clkrate_change_pending));
		return -EAGAIN;
	}

	return 0;
}

static int dsi_bridge_step_refresh_verify_clock(struct dsi_bridge *bridge,
		u32 expected_rate)
{
	struct dsi_display *display = bridge->display;
	struct dsi_display_ctrl *m_ctrl;
	struct clk *byte_clk;
	u64 actual_rate;
	u64 configured_rate;
	u64 expected_clk;

	if (!display || !display->panel || !display->panel->cur_mode)
		return -EAGAIN;

	if (display->panel->cur_mode->timing.refresh_rate != expected_rate)
		return -EAGAIN;

	expected_clk = display->panel->cur_mode->timing.clk_rate_hz;
	m_ctrl = &display->ctrl[display->clk_master_idx];
	if (!m_ctrl->ctrl || !m_ctrl->ctrl->clk_info.hs_link_clks.byte_clk)
		return -ENODEV;

	byte_clk = m_ctrl->ctrl->clk_info.hs_link_clks.byte_clk;
	configured_rate = m_ctrl->ctrl->clk_freq.byte_clk_rate * 8ULL;
	actual_rate = clk_get_rate(byte_clk) * 8ULL;
	if (!expected_clk || configured_rate != expected_clk ||
	    actual_rate != expected_clk) {
		pr_warn("[step90] %u Hz link clock mismatch: expected=%llu configured=%llu actual=%llu\n",
			expected_rate, expected_clk, configured_rate,
			actual_rate);
		return -EAGAIN;
	}

	pr_info("[step90] %u Hz link clock verified: configured=%llu actual=%llu Hz\n",
		expected_rate, configured_rate, actual_rate);
	return 0;
}

static int dsi_bridge_step_refresh_wait(struct dsi_bridge *bridge,
		struct drm_crtc *crtc, u32 expected_rate, u32 generation)
{
	u32 before, after, last;
	int rc;
	int i;

	if (!dsi_bridge_step_refresh_generation_current(bridge, generation))
		return -ECANCELED;

	rc = dsi_bridge_step_refresh_check_ready(bridge, crtc,
			expected_rate, false);
	if (rc)
		return rc;

	before = drm_crtc_vblank_count(crtc);
	last = before;
	for (i = 0; i < STEP_REFRESH_VBLANK_COUNT; i++) {
		drm_crtc_wait_one_vblank(crtc);
		if (!dsi_bridge_step_refresh_generation_current(
				bridge, generation))
			return -ECANCELED;

		after = drm_crtc_vblank_count(crtc);
		if (after == last) {
			pr_warn("[step90] %u Hz TE/vblank %d/%d did not advance: %u\n",
				expected_rate, i + 1,
				STEP_REFRESH_VBLANK_COUNT, after);
			return -EAGAIN;
		}
		last = after;
	}

	pr_info("[step90] %u Hz TE/vblank check passed: %u -> %u\n",
		expected_rate, before, last);

	return dsi_bridge_step_refresh_check_ready(bridge, crtc,
			expected_rate, false);
}

static int dsi_bridge_step_refresh_commit(struct dsi_bridge *bridge,
		u32 expected_flags, u32 expected_rate, u32 expected_generation,
		bool force_modeset, bool restart)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state = NULL;
	struct drm_crtc_state *crtc_state;
	struct drm_encoder *encoder;
	struct drm_crtc *crtc;
	struct drm_device *dev;
	u32 current_flags, current_rate, requested_rate;
	int rc = 0, attempt = 0;
	int stage_rc;

	if (!bridge || !bridge->display || !bridge->base.encoder)
		return -EINVAL;

	encoder = bridge->base.encoder;
	dev = encoder->dev;
	drm_modeset_acquire_init(&ctx, 0);

retry:
	rc = drm_modeset_lock_all_ctx(dev, &ctx);
	if (rc == -EDEADLK)
		goto deadlock;
	if (rc)
		goto out;

	crtc = encoder->crtc;
	if (!crtc || !crtc->state || !crtc->state->active ||
	    !dsi_bridge_step_refresh_generation_current(bridge,
		    expected_generation)) {
		rc = -ECANCELED;
		goto out;
	}

	current_flags = crtc->state->adjusted_mode.private_flags &
			DSI_MODE_FLAG_STEP_REFRESH_ACTIVE;
	current_rate = dsi_bridge_mode_vrefresh(
			&crtc->state->adjusted_mode);
	if (current_flags != expected_flags || current_rate != expected_rate) {
		pr_info("[step90] cancel stale stage: expected=%u/0x%x current=%u/0x%x\n",
			expected_rate, expected_flags, current_rate, current_flags);
		rc = -ECANCELED;
		goto out;
	}

	requested_rate = dsi_bridge_mode_vrefresh(&crtc->state->mode);
	if (requested_rate !=
		bridge->display->panel->step_refresh_target_rate ||
	    (restart &&
	     !atomic_read(&bridge->step_refresh_restart_pending))) {
		rc = -ECANCELED;
		goto out;
	}

	if (atomic_read(&bridge->step_refresh_blocked) ||
	    !bridge->display->panel->panel_initialized ||
	    dsi_bridge_step_refresh_panel_in_lp(bridge->display->panel) ||
	    atomic_read(&bridge->display->panel->esd_recovery_pending) ||
	    (force_modeset &&
	     atomic_read(&bridge->display->clkrate_change_pending)) ||
	    !dsi_bridge_step_refresh_host_ready(bridge->display)) {
		rc = atomic_read(&bridge->display->panel->esd_recovery_pending) ?
			-EIO : -EAGAIN;
		goto out;
	}

	state = drm_atomic_helper_duplicate_state(dev, &ctx);
	if (IS_ERR(state)) {
		rc = PTR_ERR(state);
		state = NULL;
		if (rc == -EDEADLK)
			goto deadlock;
		goto out;
	}

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state) {
		rc = -EINVAL;
		goto out;
	}
	if (!dsi_bridge_step_refresh_generation_current(bridge,
			expected_generation)) {
		rc = -ECANCELED;
		goto out;
	}

	state->allow_modeset = force_modeset;
	crtc_state->mode_changed = force_modeset;
	pr_info("[step90] %s atomic commit: requested=%u current_hw=%u flags=0x%x generation=%u\n",
		force_modeset ? "stage" : "clock-flush",
		dsi_bridge_mode_vrefresh(&crtc_state->mode), current_rate,
		current_flags, expected_generation);
	if (!dsi_bridge_step_refresh_generation_current(bridge,
			expected_generation)) {
		rc = -ECANCELED;
		goto out;
	}
	rc = drm_atomic_helper_commit_duplicated_state(state, &ctx);
	drm_atomic_state_put(state);
	state = NULL;
	if (rc == -EDEADLK)
		goto deadlock;
	stage_rc = atomic_read(&bridge->step_refresh_stage_error);
	if (!rc && stage_rc) {
		rc = stage_rc;
		pr_err("[step90] atomic stage completed with bridge error rc=%d\n",
			rc);
	}
	if (rc)
		pr_err("[step90] forced atomic modeset failed, rc=%d\n", rc);
	else
		pr_info("[step90] forced atomic modeset completed\n");
	goto out;

deadlock:
	if (state) {
		drm_atomic_state_put(state);
		state = NULL;
	}
	drm_modeset_backoff(&ctx);
	if (++attempt >= STEP_REFRESH_LOCK_RETRIES)
		goto out;
	goto retry;

out:
	if (state)
		drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return rc;
}

static void dsi_bridge_step_refresh_retry(struct dsi_bridge *bridge,
		u32 generation, u32 rate, int rc, const char *action)
{
	unsigned long irq_flags;
	u32 retry_count = 0;
	bool blocked = false;
	bool queued = false;

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	if (!bridge->step_refresh_shutdown &&
	    bridge->step_refresh_generation == generation &&
	    !atomic_read(&bridge->step_refresh_blocked) &&
	    (bridge->step_refresh_expected_flags ||
	     atomic_read(&bridge->step_refresh_restart_pending))) {
		if (bridge->step_refresh_retry_count >=
				STEP_REFRESH_READY_RETRIES) {
			atomic_set(&bridge->step_refresh_blocked, 1);
			blocked = true;
		} else {
			bridge->step_refresh_retry_count++;
			retry_count = bridge->step_refresh_retry_count;
			mod_delayed_work(system_unbound_wq,
				&bridge->step_refresh_work,
				msecs_to_jiffies(
					STEP_REFRESH_RETRY_DELAY_MS));
			queued = true;
		}
	}
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);

	if (queued)
		pr_warn("[step90] retry %s at %u Hz (%u/%u), rc=%d generation=%u\n",
			action, rate, retry_count,
			STEP_REFRESH_READY_RETRIES, rc, generation);
	else if (blocked)
		pr_err("[step90] stop %s at %u Hz after %u retries, rc=%d generation=%u\n",
			action, rate, STEP_REFRESH_READY_RETRIES,
			rc, generation);
}

static void dsi_bridge_step_refresh_fail(struct dsi_bridge *bridge,
		u32 generation, u32 rate, int rc, const char *action)
{
	unsigned long irq_flags;
	bool blocked = false;

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	if (!bridge->step_refresh_shutdown &&
	    bridge->step_refresh_generation == generation) {
		atomic_set(&bridge->step_refresh_blocked, 1);
		blocked = true;
	}
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);

	if (blocked)
		pr_err("[step90] hard failure during %s at %u Hz, rc=%d generation=%u\n",
			action, rate, rc, generation);
}

static void dsi_bridge_step_refresh_complete(struct dsi_bridge *bridge,
		u32 generation, u32 rate)
{
	unsigned long irq_flags;
	bool complete = false;

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	if (!bridge->step_refresh_shutdown &&
	    bridge->step_refresh_generation == generation) {
		bridge->step_refresh_retry_count = 0;
		atomic_set(&bridge->step_refresh_blocked, 0);
		atomic_set(&bridge->step_refresh_restart_pending, 0);
		atomic_set(&bridge->step_refresh_stage_error, 0);
		complete = true;
	}
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);

	if (complete)
		pr_info("[step90] target %u Hz verified, generation=%u\n",
			rate, generation);
}

static void dsi_bridge_step_refresh_work_fn(struct work_struct *work)
{
	struct dsi_bridge *bridge = container_of(to_delayed_work(work),
			struct dsi_bridge, step_refresh_work);
	struct dsi_display *display = bridge->display;
	struct drm_crtc *crtc;
	unsigned long irq_flags;
	u32 flags, rate, generation;
	bool restart;
	int rc;

	if (!dsi_bridge_step_refresh_snapshot(bridge, &flags, &rate,
			&generation, NULL, &restart) ||
	    atomic_read(&bridge->step_refresh_blocked) ||
	    (!(flags & DSI_MODE_FLAG_STEP_REFRESH_ACTIVE) && !restart))
		return;

	crtc = bridge->base.encoder ? bridge->base.encoder->crtc : NULL;
	rc = dsi_bridge_step_refresh_check_ready(bridge, crtc, rate, true);
	if (rc) {
		if (rc == -EAGAIN)
			dsi_bridge_step_refresh_retry(bridge, generation,
				rate, rc, "hardware readiness");
		else if (rc != -ECANCELED && rc != -ESHUTDOWN)
			dsi_bridge_step_refresh_fail(bridge, generation,
				rate, rc, "hardware readiness");
		return;
	}

	if (!dsi_bridge_step_refresh_generation_current(bridge, generation))
		return;

	if (atomic_read(&display->clkrate_change_pending)) {
		pr_info("[step90] %u Hz clock update pending; issue no-op commit generation=%u\n",
			rate, generation);
		rc = dsi_bridge_step_refresh_commit(bridge, flags, rate,
				generation, false, restart);
		if (rc) {
			if (rc == -EAGAIN || rc == -EDEADLK)
				dsi_bridge_step_refresh_retry(bridge,
					generation, rate, rc,
					"clock-flush commit");
			else if (rc != -ECANCELED && rc != -ESHUTDOWN)
				dsi_bridge_step_refresh_fail(bridge,
					generation, rate, rc,
					"clock-flush commit");
			return;
		}

		if (!dsi_bridge_step_refresh_generation_current(
				bridge, generation))
			return;

		if (atomic_read(&display->clkrate_change_pending)) {
			dsi_bridge_step_refresh_retry(bridge, generation,
				rate, -EAGAIN, "clock-flush completion");
			return;
		}

		spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
		if (!bridge->step_refresh_shutdown &&
		    bridge->step_refresh_generation == generation)
			bridge->step_refresh_retry_count = 0;
		spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);
		pr_info("[step90] %u Hz link clock update completed generation=%u\n",
			rate, generation);
	}

	if ((flags & DSI_MODE_FLAG_STEP_REFRESH_FINAL) && !restart) {
		rc = dsi_bridge_step_refresh_verify_clock(bridge, rate);
		if (rc) {
			if (rc == -EAGAIN)
				dsi_bridge_step_refresh_retry(bridge, generation,
					rate, rc, "target clock verification");
			else if (rc != -ECANCELED && rc != -ESHUTDOWN)
				dsi_bridge_step_refresh_fail(bridge, generation,
					rate, rc, "target clock verification");
			return;
		}
	}

	rc = dsi_bridge_step_refresh_wait(bridge, crtc, rate, generation);
	if (rc) {
		if (rc == -EAGAIN)
			dsi_bridge_step_refresh_retry(bridge, generation,
				rate, rc, "TE/vblank readiness");
		else if (rc != -ECANCELED && rc != -ESHUTDOWN)
			dsi_bridge_step_refresh_fail(bridge, generation,
				rate, rc, "TE/vblank readiness");
		return;
	}

	if ((flags & DSI_MODE_FLAG_STEP_REFRESH_FINAL) && !restart) {
		dsi_bridge_step_refresh_complete(bridge, generation, rate);
		return;
	}

	rc = dsi_bridge_step_refresh_commit(bridge, flags, rate,
			generation, true, restart);
	if (rc == -EAGAIN || rc == -EDEADLK)
		dsi_bridge_step_refresh_retry(bridge, generation,
			rate, rc, "stage commit");
	else if (rc && rc != -ECANCELED && rc != -ESHUTDOWN)
		dsi_bridge_step_refresh_fail(bridge, generation,
			rate, rc, "stage commit");
}

static void dsi_bridge_schedule_step_refresh(struct dsi_bridge *bridge,
		struct drm_connector *connector)
{
	struct drm_crtc *crtc;
	struct drm_display_mode *mode;
	unsigned long irq_flags;
	u32 flags, rate, requested_rate, generation = 0;
	bool blocked = false;
	bool queued = false;
	bool restart;
	bool state_changed;
	int stage_error = 0;

	if (!bridge || !bridge->display || !bridge->display->panel ||
	    !bridge->display->panel->step_refresh_enabled ||
	    !connector || !connector->state || !connector->state->crtc ||
	    !connector->state->crtc->state)
		return;

	crtc = connector->state->crtc;
	mode = &crtc->state->adjusted_mode;
	flags = mode->private_flags & DSI_MODE_FLAG_STEP_REFRESH_ACTIVE;
	rate = dsi_bridge_mode_vrefresh(mode);
	requested_rate = dsi_bridge_mode_vrefresh(&crtc->state->mode);

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	if (bridge->step_refresh_shutdown)
		goto unlock;

	restart = atomic_read(&bridge->step_refresh_restart_pending);
	if (restart &&
	    requested_rate !=
		bridge->display->panel->step_refresh_target_rate) {
		atomic_set(&bridge->step_refresh_restart_pending, 0);
		restart = false;
	}

	state_changed = bridge->step_refresh_expected_flags != flags ||
		bridge->step_refresh_expected_rate != rate;

	/*
	 * A clock-flush commit duplicates the current state and also reaches
	 * post_kickoff. Keep the LP restart armed for that no-op commit; only a
	 * newly landed hardware stage may hand control back to the normal chain.
	 */
	if (restart && state_changed &&
	    ((rate == bridge->display->panel->step_refresh_base_rate &&
	      (flags & DSI_MODE_FLAG_STEP_REFRESH_NEXT_BRIDGE)) ||
	     (rate == bridge->display->panel->step_refresh_bridge_rate &&
	      (flags & DSI_MODE_FLAG_STEP_REFRESH_NEXT_TARGET)))) {
		atomic_set(&bridge->step_refresh_restart_pending, 0);
		restart = false;
	}

	if (state_changed) {
		bridge->step_refresh_expected_flags = flags;
		bridge->step_refresh_expected_rate = rate;
		bridge->step_refresh_retry_count = 0;
		bridge->step_refresh_generation++;
	}
	generation = bridge->step_refresh_generation;

	if (!flags && !restart) {
		atomic_set(&bridge->step_refresh_blocked, 0);
		atomic_set(&bridge->step_refresh_stage_error, 0);
		goto unlock;
	}

	stage_error = atomic_read(&bridge->step_refresh_stage_error);
	if (stage_error) {
		atomic_set(&bridge->step_refresh_blocked, 1);
		atomic_set(&bridge->step_refresh_restart_pending, 0);
		blocked = true;
		goto unlock;
	}

	blocked = atomic_read(&bridge->step_refresh_blocked);
	if (state_changed && !blocked) {
		mod_delayed_work(system_unbound_wq, &bridge->step_refresh_work,
			msecs_to_jiffies(STEP_REFRESH_QUEUE_DELAY_MS));
		queued = true;
	}

unlock:
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);

	if (stage_error)
		pr_err("[step90] stop staged transition at %u Hz flags=0x%x after bridge error rc=%d generation=%u\n",
			rate, flags, stage_error, generation);
	else if (queued)
		pr_info("[step90] queued stage at %u Hz flags=0x%x restart=%d generation=%u\n",
			rate, flags, restart, generation);
	else if (blocked && state_changed)
		pr_warn_ratelimited("[step90] stage at %u Hz flags=0x%x blocked after prior hard failure\n",
			rate, flags);
}

static void convert_to_dsi_mode(const struct drm_display_mode *drm_mode,
				struct dsi_display_mode *dsi_mode)
{
	memset(dsi_mode, 0, sizeof(*dsi_mode));

	dsi_mode->timing.h_active = drm_mode->hdisplay;
	dsi_mode->timing.h_back_porch = drm_mode->htotal - drm_mode->hsync_end;
	dsi_mode->timing.h_sync_width = drm_mode->htotal -
			(drm_mode->hsync_start + dsi_mode->timing.h_back_porch);
	dsi_mode->timing.h_front_porch = drm_mode->hsync_start -
					 drm_mode->hdisplay;
	dsi_mode->timing.h_skew = drm_mode->hskew;

	dsi_mode->timing.v_active = drm_mode->vdisplay;
	dsi_mode->timing.v_back_porch = drm_mode->vtotal - drm_mode->vsync_end;
	dsi_mode->timing.v_sync_width = drm_mode->vtotal -
		(drm_mode->vsync_start + dsi_mode->timing.v_back_porch);

	dsi_mode->timing.v_front_porch = drm_mode->vsync_start -
					 drm_mode->vdisplay;

	dsi_mode->timing.refresh_rate = drm_mode->vrefresh;

	dsi_mode->pixel_clk_khz = drm_mode->clock;

	dsi_mode->priv_info =
		(struct dsi_display_mode_priv_info *)drm_mode->private;

	if (dsi_mode->priv_info) {
		dsi_mode->timing.dsc_enabled = dsi_mode->priv_info->dsc_enabled;
		dsi_mode->timing.dsc = &dsi_mode->priv_info->dsc;
	}

	if (msm_is_mode_seamless(drm_mode))
		dsi_mode->dsi_mode_flags |= DSI_MODE_FLAG_SEAMLESS;
	if (msm_is_mode_dynamic_fps(drm_mode))
		dsi_mode->dsi_mode_flags |= DSI_MODE_FLAG_DFPS;
	if (msm_needs_vblank_pre_modeset(drm_mode))
		dsi_mode->dsi_mode_flags |= DSI_MODE_FLAG_VBLANK_PRE_MODESET;
	if (msm_is_mode_seamless_dms(drm_mode))
		dsi_mode->dsi_mode_flags |= DSI_MODE_FLAG_DMS;
	if (msm_is_mode_seamless_vrr(drm_mode))
		dsi_mode->dsi_mode_flags |= DSI_MODE_FLAG_VRR;
	if (msm_is_mode_seamless_poms(drm_mode))
		dsi_mode->dsi_mode_flags |= DSI_MODE_FLAG_POMS;
	if (msm_is_mode_seamless_dyn_clk(drm_mode))
		dsi_mode->dsi_mode_flags |= DSI_MODE_FLAG_DYN_CLK;

	dsi_mode->timing.h_sync_polarity =
			!!(drm_mode->flags & DRM_MODE_FLAG_PHSYNC);
	dsi_mode->timing.v_sync_polarity =
			!!(drm_mode->flags & DRM_MODE_FLAG_PVSYNC);

	if (drm_mode->flags & DRM_MODE_FLAG_VID_MODE_PANEL)
		dsi_mode->panel_mode = DSI_OP_VIDEO_MODE;
	if (drm_mode->flags & DRM_MODE_FLAG_CMD_MODE_PANEL)
		dsi_mode->panel_mode = DSI_OP_CMD_MODE;
}

void dsi_convert_to_drm_mode(const struct dsi_display_mode *dsi_mode,
				struct drm_display_mode *drm_mode)
{
	memset(drm_mode, 0, sizeof(*drm_mode));

	drm_mode->hdisplay = dsi_mode->timing.h_active;
	drm_mode->hsync_start = drm_mode->hdisplay +
				dsi_mode->timing.h_front_porch;
	drm_mode->hsync_end = drm_mode->hsync_start +
			      dsi_mode->timing.h_sync_width;
	drm_mode->htotal = drm_mode->hsync_end + dsi_mode->timing.h_back_porch;
	drm_mode->hskew = dsi_mode->timing.h_skew;

	drm_mode->vdisplay = dsi_mode->timing.v_active;
	drm_mode->vsync_start = drm_mode->vdisplay +
				dsi_mode->timing.v_front_porch;
	drm_mode->vsync_end = drm_mode->vsync_start +
			      dsi_mode->timing.v_sync_width;
	drm_mode->vtotal = drm_mode->vsync_end + dsi_mode->timing.v_back_porch;

	drm_mode->vrefresh = dsi_mode->timing.refresh_rate;
	drm_mode->clock = dsi_mode->pixel_clk_khz;

	drm_mode->private = (int *)dsi_mode->priv_info;

	if (dsi_mode->dsi_mode_flags & DSI_MODE_FLAG_SEAMLESS)
		drm_mode->flags |= DRM_MODE_FLAG_SEAMLESS;
	if (dsi_mode->dsi_mode_flags & DSI_MODE_FLAG_DFPS)
		drm_mode->private_flags |= MSM_MODE_FLAG_SEAMLESS_DYNAMIC_FPS;
	if (dsi_mode->dsi_mode_flags & DSI_MODE_FLAG_VBLANK_PRE_MODESET)
		drm_mode->private_flags |= MSM_MODE_FLAG_VBLANK_PRE_MODESET;
	if (dsi_mode->dsi_mode_flags & DSI_MODE_FLAG_DMS)
		drm_mode->private_flags |= MSM_MODE_FLAG_SEAMLESS_DMS;
	if (dsi_mode->dsi_mode_flags & DSI_MODE_FLAG_VRR)
		drm_mode->private_flags |= MSM_MODE_FLAG_SEAMLESS_VRR;
	if (dsi_mode->dsi_mode_flags & DSI_MODE_FLAG_POMS)
		drm_mode->private_flags |= MSM_MODE_FLAG_SEAMLESS_POMS;
	if (dsi_mode->dsi_mode_flags & DSI_MODE_FLAG_DYN_CLK)
		drm_mode->private_flags |= MSM_MODE_FLAG_SEAMLESS_DYN_CLK;

	if (dsi_mode->timing.h_sync_polarity)
		drm_mode->flags |= DRM_MODE_FLAG_PHSYNC;
	if (dsi_mode->timing.v_sync_polarity)
		drm_mode->flags |= DRM_MODE_FLAG_PVSYNC;

	if (dsi_mode->panel_mode == DSI_OP_VIDEO_MODE)
		drm_mode->flags |= DRM_MODE_FLAG_VID_MODE_PANEL;
	if (dsi_mode->panel_mode == DSI_OP_CMD_MODE)
		drm_mode->flags |= DRM_MODE_FLAG_CMD_MODE_PANEL;

	/* set mode name */
	snprintf(drm_mode->name, DRM_DISPLAY_MODE_LEN, "%dx%dx%dx%d",
			drm_mode->hdisplay, drm_mode->vdisplay,
			drm_mode->vrefresh, drm_mode->clock);
}

static int dsi_bridge_attach(struct drm_bridge *bridge)
{
	struct dsi_bridge *c_bridge = to_dsi_bridge(bridge);

	if (!bridge) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	pr_debug("[%d] attached\n", c_bridge->id);

	return 0;

}

static void dsi_bridge_pre_enable(struct drm_bridge *bridge)
{
	int rc = 0;
	struct dsi_bridge *c_bridge = to_dsi_bridge(bridge);
	struct msm_drm_notifier notify_data;
	int power_mode;

	if (!bridge) {
		pr_err("Invalid params\n");
		return;
	}

	if (!c_bridge || !c_bridge->display || !c_bridge->display->panel) {
		pr_err("Incorrect bridge details\n");
		return;
	}
	if (dsi_bridge_step_refresh_active_flags(c_bridge))
		atomic_set(&c_bridge->step_refresh_stage_error, 0);

	if (bridge->encoder->crtc->state->active_changed)
		atomic_set(&c_bridge->display->panel->esd_recovery_pending, 0);
	if (bridge->encoder->crtc->state->active_changed &&
	    c_bridge->display->panel->step_refresh_enabled) {
		dsi_bridge_invalidate_step_refresh(c_bridge);
		atomic_set(&c_bridge->step_refresh_blocked, 0);
		pr_info("[step90] display activation invalidates old work and clears prior failure\n");
	}

	power_mode = sde_connector_get_lp(c_bridge->display->drm_conn);
	notify_data.data = &power_mode;
	notify_data.id = MSM_DRM_PRIMARY_DISPLAY;
	msm_drm_notifier_call_chain(MSM_DRM_EARLY_EVENT_BLANK, &notify_data);

	/* By this point mode should have been validated through mode_fixup */
	/* DIAG: trace the enable path decision. */
	pr_info("[endiag] pre_enable: active_changed=%d flags=0x%x refresh=%u\n",
		bridge->encoder->crtc->state->active_changed,
		c_bridge->dsi_mode.dsi_mode_flags,
		c_bridge->dsi_mode.timing.refresh_rate);
	rc = dsi_display_set_mode(c_bridge->display,
			&(c_bridge->dsi_mode), 0x0);
	if (rc) {
		pr_err("[%d] failed to perform a mode set, rc=%d\n",
		       c_bridge->id, rc);
		dsi_bridge_step_refresh_latch_error(c_bridge, rc, "mode set");
		return;
	}

	if (c_bridge->dsi_mode.dsi_mode_flags &
		(DSI_MODE_FLAG_SEAMLESS | DSI_MODE_FLAG_VRR |
		 DSI_MODE_FLAG_DYN_CLK)) {
		pr_info("[endiag] seamless path, skip prepare/enable\n");
		return;
	}

	pr_info("[endiag] full path: prepare + enable\n");

	SDE_ATRACE_BEGIN("dsi_display_prepare");
	rc = dsi_display_prepare(c_bridge->display);
	if (rc) {
		pr_err("[%d] DSI display prepare failed, rc=%d\n",
		       c_bridge->id, rc);
		dsi_bridge_step_refresh_latch_error(c_bridge, rc,
			"display prepare");
		SDE_ATRACE_END("dsi_display_prepare");
		return;
	}
	SDE_ATRACE_END("dsi_display_prepare");

	SDE_ATRACE_BEGIN("dsi_display_enable");
	rc = dsi_display_enable(c_bridge->display);
	if (rc) {
		pr_err("[%d] DSI display enable failed, rc=%d\n",
				c_bridge->id, rc);
		dsi_bridge_step_refresh_latch_error(c_bridge, rc,
			"display enable");
		(void)dsi_display_unprepare(c_bridge->display);
	}
	SDE_ATRACE_END("dsi_display_enable");

	msm_drm_notifier_call_chain(MSM_DRM_EVENT_BLANK, &notify_data);

	rc = dsi_display_splash_res_cleanup(c_bridge->display);
	if (rc)
		pr_err("Continuous splash pipeline cleanup failed, rc=%d\n",
									rc);
}

static void dsi_bridge_enable(struct drm_bridge *bridge)
{
	int rc = 0;
	struct dsi_bridge *c_bridge = to_dsi_bridge(bridge);
	struct dsi_display *display;

	if (!bridge) {
		pr_err("Invalid params\n");
		return;
	}

	if (c_bridge->dsi_mode.dsi_mode_flags &
			(DSI_MODE_FLAG_SEAMLESS | DSI_MODE_FLAG_VRR |
			 DSI_MODE_FLAG_DYN_CLK)) {
		pr_debug("[%d] seamless enable\n", c_bridge->id);
		return;
	}
	if (dsi_bridge_step_refresh_active_flags(c_bridge) &&
	    atomic_read(&c_bridge->step_refresh_stage_error)) {
		pr_err("[step90] skip post-enable after staged bridge error rc=%d\n",
			atomic_read(&c_bridge->step_refresh_stage_error));
		return;
	}
	display = c_bridge->display;

	rc = dsi_display_post_enable(display);
	if (rc) {
		pr_err("[%d] DSI display post enabled failed, rc=%d\n",
		       c_bridge->id, rc);
		dsi_bridge_step_refresh_latch_error(c_bridge, rc,
			"display post-enable");
		if (dsi_bridge_step_refresh_active_flags(c_bridge))
			return;
	}

	if (display && display->drm_conn) {
		sde_connector_helper_bridge_enable(display->drm_conn);
		if (c_bridge->dsi_mode.dsi_mode_flags & DSI_MODE_FLAG_POMS)
			sde_connector_schedule_status_work(display->drm_conn,
					true);
	}

#if defined(CONFIG_MACH_XIAOMI_VAYU) || defined(CONFIG_MACH_XIAOMI_NABU)
	dsi_display_esd_irq_ctrl(display, true);
#endif
}

static void dsi_bridge_disable(struct drm_bridge *bridge)
{
	int rc = 0;
	struct dsi_display *display;
	struct dsi_bridge *c_bridge = to_dsi_bridge(bridge);
	int private_flags;
	struct msm_drm_notifier notify_data;
	int power_mode;

	if (!bridge) {
		pr_err("Invalid params\n");
		return;
	}

	if (c_bridge->display && c_bridge->display->panel &&
	    c_bridge->display->panel->step_refresh_enabled) {
		pr_info("[step90] display disable invalidates staged transition\n");
		dsi_bridge_invalidate_step_refresh(c_bridge);
	}

	power_mode = sde_connector_get_lp(c_bridge->display->drm_conn);
	notify_data.data = &power_mode;
	notify_data.id = MSM_DRM_PRIMARY_DISPLAY;
	msm_drm_notifier_call_chain(MSM_DRM_R_EARLY_EVENT_BLANK, &notify_data);

	display = c_bridge->display;
	if (display && display->panel &&
	    display->panel->step_refresh_enabled &&
	    bridge->encoder && bridge->encoder->crtc &&
	    bridge->encoder->crtc->state &&
	    !bridge->encoder->crtc->state->active)
		dsi_bridge_invalidate_step_refresh(c_bridge);

#if defined(CONFIG_MACH_XIAOMI_VAYU) || defined(CONFIG_MACH_XIAOMI_NABU)
	dsi_display_esd_irq_ctrl(display, false);
#endif

	private_flags =
		bridge->encoder->crtc->state->adjusted_mode.private_flags;

	if (display && display->drm_conn) {
		display->poms_pending =
			private_flags & MSM_MODE_FLAG_SEAMLESS_POMS;
		sde_connector_helper_bridge_disable(display->drm_conn);
	}

	rc = dsi_display_pre_disable(c_bridge->display);
	if (rc) {
		pr_err("[%d] DSI display pre disable failed, rc=%d\n",
		       c_bridge->id, rc);
	}
}

static void dsi_bridge_post_disable(struct drm_bridge *bridge)
{
	int rc = 0;
	struct dsi_bridge *c_bridge = to_dsi_bridge(bridge);
	struct msm_drm_notifier notify_data;
	int power_mode;

	if (!bridge) {
		pr_err("Invalid params\n");
		return;
	}

	power_mode = sde_connector_get_lp(c_bridge->display->drm_conn);
	notify_data.data = &power_mode;
	notify_data.id = MSM_DRM_PRIMARY_DISPLAY;
	msm_drm_notifier_call_chain(MSM_DRM_EARLY_EVENT_BLANK, &notify_data);

	SDE_ATRACE_BEGIN("dsi_bridge_post_disable");
	SDE_ATRACE_BEGIN("dsi_display_disable");
	rc = dsi_display_disable(c_bridge->display);
	if (rc) {
		pr_err("[%d] DSI display disable failed, rc=%d\n",
		       c_bridge->id, rc);
		SDE_ATRACE_END("dsi_display_disable");
		return;
	}
	SDE_ATRACE_END("dsi_display_disable");

	rc = dsi_display_unprepare(c_bridge->display);
	if (rc) {
		pr_err("[%d] DSI display unprepare failed, rc=%d\n",
		       c_bridge->id, rc);
		SDE_ATRACE_END("dsi_bridge_post_disable");
		return;
	}
	SDE_ATRACE_END("dsi_bridge_post_disable");

	msm_drm_notifier_call_chain(MSM_DRM_EVENT_BLANK, &notify_data);
}

static void dsi_bridge_mode_set(struct drm_bridge *bridge,
				struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	struct dsi_bridge *c_bridge = to_dsi_bridge(bridge);

	if (!bridge || !mode || !adjusted_mode) {
		pr_err("Invalid params\n");
		return;
	}

	memset(&(c_bridge->dsi_mode), 0x0, sizeof(struct dsi_display_mode));
	convert_to_dsi_mode(adjusted_mode, &(c_bridge->dsi_mode));

	/* restore bit_clk_rate also for dynamic clk use cases */
	c_bridge->dsi_mode.timing.clk_rate_hz =
		dsi_drm_find_bit_clk_rate(c_bridge->display, adjusted_mode);

	pr_debug("clk_rate: %llu\n", c_bridge->dsi_mode.timing.clk_rate_hz);
}

static bool dsi_bridge_mode_fixup(struct drm_bridge *bridge,
				  const struct drm_display_mode *mode,
				  struct drm_display_mode *adjusted_mode)
{
	int rc = 0;
	struct dsi_bridge *c_bridge;
	struct dsi_display *display;
	struct dsi_display_mode dsi_mode, cur_dsi_mode, *panel_dsi_mode;
	struct dsi_display_mode *step_dsi_mode;
	struct drm_display_mode cur_mode, stage_mode;
	const struct drm_display_mode *cur_hw_mode;
	struct drm_crtc_state *crtc_state;
	bool clone_mode = false;
	bool have_current_mode = false;
	bool old_active = false;
	bool restart = false;
	struct drm_encoder *encoder;
	u32 current_hw_rate = 0, target_rate, step_rate;
	u32 step_flag = 0, saved_mode_flags;

	if (!bridge || !mode || !adjusted_mode) {
		pr_err("Invalid params\n");
		return false;
	}

	c_bridge = to_dsi_bridge(bridge);
	crtc_state = container_of(mode, struct drm_crtc_state, mode);
	display = c_bridge->display;
	if (!display) {
		pr_err("Invalid params\n");
		return false;
	}

	/*
	 * DRM calls bridge mode_fixup for plane-only and duplicated-state
	 * commits too. In those commits adjusted_mode already describes the
	 * active hardware stage; recalculating it here would advance the staged
	 * chain in software without running bridge mode_set or a panel switch.
	 */
	if (display->panel && display->panel->step_refresh_enabled &&
	    !crtc_state->mode_changed)
		return true;

	/*
	 * if no timing defined in panel, it must be external mode
	 * and we'll use empty priv info to populate the mode
	 */
	if (display->panel && !display->panel->num_timing_nodes) {
		*adjusted_mode = *mode;
		adjusted_mode->private = (int *)&default_priv_info;
		adjusted_mode->private_flags = 0;
		return true;
	}

	convert_to_dsi_mode(mode, &dsi_mode);

	/*
	 * retrieve dsi mode from dsi driver's cache since not safe to take
	 * the drm mode config mutex in all paths
	 */
	rc = dsi_display_find_mode(display, &dsi_mode, &panel_dsi_mode);
	if (rc)
		return false;

	/* propagate the private info to the adjusted_mode derived dsi mode */
	dsi_mode.priv_info = panel_dsi_mode->priv_info;
	dsi_mode.dsi_mode_flags = panel_dsi_mode->dsi_mode_flags;
	if (!dsi_mode.priv_info) {
		pr_err("[%d] mode has no private timing data\n", c_bridge->id);
		return false;
	}
	dsi_mode.timing.dsc_enabled = dsi_mode.priv_info->dsc_enabled;
	dsi_mode.timing.dsc = &dsi_mode.priv_info->dsc;
	target_rate = dsi_mode.timing.refresh_rate;
	step_rate = target_rate;

	if (bridge->encoder && bridge->encoder->crtc &&
		    crtc_state->crtc && crtc_state->crtc->state) {
		cur_hw_mode = &crtc_state->crtc->state->adjusted_mode;
		if (!cur_hw_mode->clock)
			cur_hw_mode = &crtc_state->crtc->state->mode;
		current_hw_rate = dsi_bridge_mode_vrefresh(cur_hw_mode);
		old_active = crtc_state->crtc->state->active;
		if (cur_hw_mode->clock) {
			convert_to_dsi_mode(cur_hw_mode, &cur_dsi_mode);
			have_current_mode = true;
		}
	}

	/*
	 * EA8076 cannot reliably accept the large 60 -> 90 oscillator and
	 * link-clock jump. Keep userspace's requested mode untouched, but make
	 * adjusted_mode describe the single hardware stage performed by this
	 * commit. The worker duplicates the same requested state for each next
	 * stage, so Android still sees one logical 60/72/84/90 mode switch.
	 */
	if (display->panel && display->panel->step_refresh_enabled &&
	    crtc_state->active &&
	    target_rate == display->panel->step_refresh_target_rate) {
		restart = atomic_read(&c_bridge->step_refresh_restart_pending);

		if (!old_active) {
			step_rate = display->panel->step_refresh_base_rate;
			step_flag = DSI_MODE_FLAG_STEP_REFRESH_NEXT_BRIDGE;
		} else if (restart) {
			if (current_hw_rate ==
					display->panel->step_refresh_base_rate) {
				step_rate =
					display->panel->step_refresh_bridge_rate;
				step_flag =
					DSI_MODE_FLAG_STEP_REFRESH_NEXT_TARGET;
			} else {
				step_rate =
					display->panel->step_refresh_base_rate;
				step_flag =
					DSI_MODE_FLAG_STEP_REFRESH_NEXT_BRIDGE;
			}
		} else if (current_hw_rate ==
				display->panel->step_refresh_base_rate) {
			step_rate = display->panel->step_refresh_bridge_rate;
			step_flag = DSI_MODE_FLAG_STEP_REFRESH_NEXT_TARGET;
		} else if (current_hw_rate ==
				display->panel->step_refresh_bridge_rate) {
			step_rate = target_rate;
			step_flag = DSI_MODE_FLAG_STEP_REFRESH_FINAL;
		} else if (current_hw_rate != target_rate) {
			step_rate = display->panel->step_refresh_base_rate;
			step_flag = DSI_MODE_FLAG_STEP_REFRESH_NEXT_BRIDGE;
		} else {
			step_flag = DSI_MODE_FLAG_STEP_REFRESH_FINAL;
		}

		if (step_rate != target_rate) {
			step_dsi_mode = dsi_bridge_find_refresh_mode(display,
					panel_dsi_mode, step_rate);
			if (!step_dsi_mode) {
				pr_err("[step90] required %u Hz hardware mode is missing\n",
					step_rate);
				return false;
			}

			saved_mode_flags = dsi_mode.dsi_mode_flags;
			dsi_mode = *step_dsi_mode;
			dsi_mode.dsi_mode_flags = saved_mode_flags;
		}
	}

	/* Validate and classify the actual hardware stage, not the final target. */
	rc = dsi_display_validate_mode(c_bridge->display, &dsi_mode,
			DSI_VALIDATE_FLAG_ALLOW_ADJUST);
	if (rc) {
		pr_err("[%d] stage %u Hz is not valid, rc=%d\n",
			c_bridge->id, step_rate, rc);
		return false;
	}

	dsi_convert_to_drm_mode(&dsi_mode, &stage_mode);

	if (have_current_mode && old_active) {
		cur_dsi_mode.timing.dsc_enabled =
				dsi_mode.priv_info->dsc_enabled;
		cur_dsi_mode.timing.dsc = &dsi_mode.priv_info->dsc;
		rc = dsi_display_validate_mode_change(c_bridge->display,
					&cur_dsi_mode, &dsi_mode);
		if (rc) {
			pr_err("[%s] seamless mode mismatch failure rc=%d\n",
				c_bridge->display->name, rc);
			return false;
		}

		drm_for_each_encoder(encoder, crtc_state->crtc->dev) {
			if (encoder->crtc != crtc_state->crtc)
				continue;

			if (sde_encoder_in_clone_mode(encoder))
				clone_mode = true;
		}

		cur_mode = *cur_hw_mode;

		/* No panel mode switch when drm pipeline is changing */
		if ((dsi_mode.panel_mode != cur_dsi_mode.panel_mode) &&
			(!(dsi_mode.dsi_mode_flags & DSI_MODE_FLAG_VRR)) &&
			(crtc_state->enable ==
				crtc_state->crtc->state->enable))
			dsi_mode.dsi_mode_flags |= DSI_MODE_FLAG_POMS;

		/* No DMS/VRR when drm pipeline is changing */
		if (!drm_mode_equal(&cur_mode, &stage_mode) &&
			(!(dsi_mode.dsi_mode_flags & DSI_MODE_FLAG_VRR)) &&
			(!(dsi_mode.dsi_mode_flags & DSI_MODE_FLAG_POMS)) &&
			(!(dsi_mode.dsi_mode_flags & DSI_MODE_FLAG_DYN_CLK)) &&
			(!crtc_state->active_changed ||
			 display->is_cont_splash_enabled))
			dsi_mode.dsi_mode_flags |= DSI_MODE_FLAG_DMS;

		/* Reject seemless transition when active/connectors changed.*/
		if ((crtc_state->active_changed ||
			(crtc_state->connectors_changed && clone_mode)) &&
			((dsi_mode.dsi_mode_flags & DSI_MODE_FLAG_VRR) ||
			(dsi_mode.dsi_mode_flags & DSI_MODE_FLAG_DYN_CLK))) {
			pr_err("seamless on active/conn(%d/%d) changed 0x%x\n",
				crtc_state->active_changed,
				crtc_state->connectors_changed,
				dsi_mode.dsi_mode_flags);
			return false;
		}
	}

	/* convert back to drm mode, propagating the private info & flags */
	dsi_convert_to_drm_mode(&dsi_mode, adjusted_mode);
	adjusted_mode->private_flags |= step_flag;

	if (step_flag)
		pr_info("[step90] request=%u old_hw=%u old_active=%d restart=%d stage=%u flags=0x%x\n",
			target_rate, current_hw_rate, old_active, restart,
			step_rate, step_flag);

	return true;
}

u64 dsi_drm_find_bit_clk_rate(void *display,
			      const struct drm_display_mode *drm_mode)
{
	int i = 0, count = 0;
	struct dsi_display *dsi_display = display;
	struct dsi_display_mode *dsi_mode;
	u64 bit_clk_rate = 0;

	if (!dsi_display || !drm_mode)
		return 0;

	dsi_display_get_mode_count(dsi_display, &count);

	for (i = 0; i < count; i++) {
		dsi_mode = &dsi_display->modes[i];
		if ((dsi_mode->timing.v_active == drm_mode->vdisplay) &&
		    (dsi_mode->timing.h_active == drm_mode->hdisplay) &&
		    (dsi_mode->pixel_clk_khz == drm_mode->clock) &&
		    (dsi_mode->timing.refresh_rate == drm_mode->vrefresh)) {
			bit_clk_rate = dsi_mode->timing.clk_rate_hz;
			break;
		}
	}

	return bit_clk_rate;
}

int dsi_conn_get_mode_info(struct drm_connector *connector,
		const struct drm_display_mode *drm_mode,
		struct msm_mode_info *mode_info,
		u32 max_mixer_width, void *display)
{
	struct dsi_display_mode dsi_mode;
	struct dsi_mode_info *timing;

	if (!drm_mode || !mode_info)
		return -EINVAL;

	convert_to_dsi_mode(drm_mode, &dsi_mode);

	if (!dsi_mode.priv_info)
		return -EINVAL;

	memset(mode_info, 0, sizeof(*mode_info));

	timing = &dsi_mode.timing;
	mode_info->frame_rate = dsi_mode.timing.refresh_rate;
	mode_info->vtotal = DSI_V_TOTAL(timing);
	mode_info->prefill_lines = dsi_mode.priv_info->panel_prefill_lines;
	mode_info->jitter_numer = dsi_mode.priv_info->panel_jitter_numer;
	mode_info->jitter_denom = dsi_mode.priv_info->panel_jitter_denom;
	mode_info->clk_rate = dsi_drm_find_bit_clk_rate(display, drm_mode);
	mode_info->mdp_transfer_time_us =
		dsi_mode.priv_info->mdp_transfer_time_us;
	mode_info->overlap_pixels = dsi_mode.priv_info->overlap_pixels;

	memcpy(&mode_info->topology, &dsi_mode.priv_info->topology,
			sizeof(struct msm_display_topology));

	mode_info->comp_info.comp_type = MSM_DISPLAY_COMPRESSION_NONE;
	if (dsi_mode.priv_info->dsc_enabled) {
		mode_info->comp_info.comp_type = MSM_DISPLAY_COMPRESSION_DSC;
		memcpy(&mode_info->comp_info.dsc_info, &dsi_mode.priv_info->dsc,
			sizeof(dsi_mode.priv_info->dsc));
		mode_info->comp_info.comp_ratio =
			MSM_DISPLAY_COMPRESSION_RATIO_3_TO_1;
	}

	if (dsi_mode.priv_info->roi_caps.enabled) {
		memcpy(&mode_info->roi_caps, &dsi_mode.priv_info->roi_caps,
			sizeof(dsi_mode.priv_info->roi_caps));
	}

	return 0;
}

static const struct drm_bridge_funcs dsi_bridge_ops = {
	.attach       = dsi_bridge_attach,
	.mode_fixup   = dsi_bridge_mode_fixup,
	.pre_enable   = dsi_bridge_pre_enable,
	.enable       = dsi_bridge_enable,
	.disable      = dsi_bridge_disable,
	.post_disable = dsi_bridge_post_disable,
	.mode_set     = dsi_bridge_mode_set,
};

int dsi_conn_set_info_blob(struct drm_connector *connector,
		void *info, void *display, struct msm_mode_info *mode_info)
{
	struct dsi_display *dsi_display = display;
	struct dsi_panel *panel;
	enum dsi_pixel_format fmt;
	u32 bpp;

	if (!info || !dsi_display)
		return -EINVAL;

	dsi_display->drm_conn = connector;

	sde_kms_info_add_keystr(info,
		"display type", dsi_display->display_type);

	switch (dsi_display->type) {
	case DSI_DISPLAY_SINGLE:
		sde_kms_info_add_keystr(info, "display config",
					"single display");
		break;
	case DSI_DISPLAY_EXT_BRIDGE:
		sde_kms_info_add_keystr(info, "display config", "ext bridge");
		break;
	case DSI_DISPLAY_SPLIT:
		sde_kms_info_add_keystr(info, "display config",
					"split display");
		break;
	case DSI_DISPLAY_SPLIT_EXT_BRIDGE:
		sde_kms_info_add_keystr(info, "display config",
					"split ext bridge");
		break;
	default:
		pr_debug("invalid display type:%d\n", dsi_display->type);
		break;
	}

	if (!dsi_display->panel) {
		pr_debug("invalid panel data\n");
		goto end;
	}

	panel = dsi_display->panel;
	sde_kms_info_add_keystr(info, "panel name", panel->name);

	switch (panel->panel_mode) {
	case DSI_OP_VIDEO_MODE:
		sde_kms_info_add_keystr(info, "panel mode", "video");
		sde_kms_info_add_keystr(info, "qsync support",
				panel->qsync_min_fps ? "true" : "false");
		break;
	case DSI_OP_CMD_MODE:
		sde_kms_info_add_keystr(info, "panel mode", "command");
		sde_kms_info_add_keyint(info, "mdp_transfer_time_us",
				mode_info->mdp_transfer_time_us);
		sde_kms_info_add_keystr(info, "qsync support",
				panel->qsync_min_fps ? "true" : "false");
		break;
	default:
		pr_debug("invalid panel type:%d\n", panel->panel_mode);
		break;
	}
	sde_kms_info_add_keystr(info, "dfps support",
			panel->dfps_caps.dfps_support ? "true" : "false");

	if (panel->dfps_caps.dfps_support) {
		sde_kms_info_add_keyint(info, "min_fps",
			panel->dfps_caps.min_refresh_rate);
		sde_kms_info_add_keyint(info, "max_fps",
			panel->dfps_caps.max_refresh_rate);
	}

	sde_kms_info_add_keystr(info, "dyn bitclk support",
			panel->dyn_clk_caps.dyn_clk_support ? "true" : "false");

	switch (panel->phy_props.rotation) {
	case DSI_PANEL_ROTATE_NONE:
		sde_kms_info_add_keystr(info, "panel orientation", "none");
		break;
	case DSI_PANEL_ROTATE_H_FLIP:
		sde_kms_info_add_keystr(info, "panel orientation", "horz flip");
		break;
	case DSI_PANEL_ROTATE_V_FLIP:
		sde_kms_info_add_keystr(info, "panel orientation", "vert flip");
		break;
	case DSI_PANEL_ROTATE_HV_FLIP:
		sde_kms_info_add_keystr(info, "panel orientation",
							"horz & vert flip");
		break;
	default:
		pr_debug("invalid panel rotation:%d\n",
						panel->phy_props.rotation);
		break;
	}

	switch (panel->bl_config.type) {
	case DSI_BACKLIGHT_PWM:
		sde_kms_info_add_keystr(info, "backlight type", "pwm");
		break;
	case DSI_BACKLIGHT_WLED:
		sde_kms_info_add_keystr(info, "backlight type", "wled");
		break;
	case DSI_BACKLIGHT_DCS:
		sde_kms_info_add_keystr(info, "backlight type", "dcs");
		break;
	default:
		pr_debug("invalid panel backlight type:%d\n",
						panel->bl_config.type);
		break;
	}

	if (mode_info && mode_info->roi_caps.enabled) {
		sde_kms_info_add_keyint(info, "partial_update_num_roi",
				mode_info->roi_caps.num_roi);
		sde_kms_info_add_keyint(info, "partial_update_xstart",
				mode_info->roi_caps.align.xstart_pix_align);
		sde_kms_info_add_keyint(info, "partial_update_walign",
				mode_info->roi_caps.align.width_pix_align);
		sde_kms_info_add_keyint(info, "partial_update_wmin",
				mode_info->roi_caps.align.min_width);
		sde_kms_info_add_keyint(info, "partial_update_ystart",
				mode_info->roi_caps.align.ystart_pix_align);
		sde_kms_info_add_keyint(info, "partial_update_halign",
				mode_info->roi_caps.align.height_pix_align);
		sde_kms_info_add_keyint(info, "partial_update_hmin",
				mode_info->roi_caps.align.min_height);
		sde_kms_info_add_keyint(info, "partial_update_roimerge",
				mode_info->roi_caps.merge_rois);
	}

	fmt = dsi_display->config.common_config.dst_format;
	bpp = dsi_ctrl_pixel_format_to_bpp(fmt);

	sde_kms_info_add_keyint(info, "bit_depth", bpp);

end:
	return 0;
}

enum drm_connector_status dsi_conn_detect(struct drm_connector *conn,
		bool force,
		void *display)
{
	enum drm_connector_status status = connector_status_unknown;
	struct msm_display_info info;
	int rc;

	if (!conn || !display)
		return status;

	/* get display dsi_info */
	memset(&info, 0x0, sizeof(info));
	rc = dsi_display_get_info(conn, &info, display);
	if (rc) {
		pr_err("failed to get display info, rc=%d\n", rc);
		return connector_status_disconnected;
	}

	if (info.capabilities & MSM_DISPLAY_CAP_HOT_PLUG)
		status = (info.is_connected ? connector_status_connected :
					      connector_status_disconnected);
	else
		status = connector_status_connected;

	conn->display_info.width_mm = info.width_mm;
	conn->display_info.height_mm = info.height_mm;

	return status;
}

void dsi_connector_put_modes(struct drm_connector *connector,
	void *display)
{
	struct drm_display_mode *drm_mode;
	struct dsi_display_mode dsi_mode;
	struct dsi_display *dsi_display;

	if (!connector || !display)
		return;

	list_for_each_entry(drm_mode, &connector->modes, head) {
		convert_to_dsi_mode(drm_mode, &dsi_mode);
		dsi_display_put_mode(display, &dsi_mode);
	}

	/* free the display structure modes also */
	dsi_display = display;
	kfree(dsi_display->modes);
	dsi_display->modes = NULL;
}


static int dsi_drm_update_edid_name(struct edid *edid, const char *name)
{
	u8 *dtd = (u8 *)&edid->detailed_timings[3];
	u8 standard_header[] = {0x00, 0x00, 0x00, 0xFE, 0x00};
	u32 dtd_size = 18;
	u32 header_size = sizeof(standard_header);

	if (!name)
		return -EINVAL;

	/* Fill standard header */
	memcpy(dtd, standard_header, header_size);

	dtd_size -= header_size;
	dtd_size = min_t(u32, dtd_size, strlen(name));

	memcpy(dtd + header_size, name, dtd_size);

	return 0;
}

static void dsi_drm_update_dtd(struct edid *edid,
		struct dsi_display_mode *modes, u32 modes_count)
{
	u32 i;
	u32 count = min_t(u32, modes_count, 3);

	for (i = 0; i < count; i++) {
		struct detailed_timing *dtd = &edid->detailed_timings[i];
		struct dsi_display_mode *mode = &modes[i];
		struct dsi_mode_info *timing = &mode->timing;
		struct detailed_pixel_timing *pd = &dtd->data.pixel_data;
		u32 h_blank = timing->h_front_porch + timing->h_sync_width +
				timing->h_back_porch;
		u32 v_blank = timing->v_front_porch + timing->v_sync_width +
				timing->v_back_porch;
		u32 h_img = 0, v_img = 0;

		dtd->pixel_clock = mode->pixel_clk_khz / 10;

		pd->hactive_lo = timing->h_active & 0xFF;
		pd->hblank_lo = h_blank & 0xFF;
		pd->hactive_hblank_hi = ((h_blank >> 8) & 0xF) |
				((timing->h_active >> 8) & 0xF) << 4;

		pd->vactive_lo = timing->v_active & 0xFF;
		pd->vblank_lo = v_blank & 0xFF;
		pd->vactive_vblank_hi = ((v_blank >> 8) & 0xF) |
				((timing->v_active >> 8) & 0xF) << 4;

		pd->hsync_offset_lo = timing->h_front_porch & 0xFF;
		pd->hsync_pulse_width_lo = timing->h_sync_width & 0xFF;
		pd->vsync_offset_pulse_width_lo =
			((timing->v_front_porch & 0xF) << 4) |
			(timing->v_sync_width & 0xF);

		pd->hsync_vsync_offset_pulse_width_hi =
			(((timing->h_front_porch >> 8) & 0x3) << 6) |
			(((timing->h_sync_width >> 8) & 0x3) << 4) |
			(((timing->v_front_porch >> 4) & 0x3) << 2) |
			(((timing->v_sync_width >> 4) & 0x3) << 0);

		pd->width_mm_lo = h_img & 0xFF;
		pd->height_mm_lo = v_img & 0xFF;
		pd->width_height_mm_hi = (((h_img >> 8) & 0xF) << 4) |
			((v_img >> 8) & 0xF);

		pd->hborder = 0;
		pd->vborder = 0;
		pd->misc = 0;
	}
}

static void dsi_drm_update_checksum(struct edid *edid)
{
	u8 *data = (u8 *)edid;
	u32 i, sum = 0;

	for (i = 0; i < EDID_LENGTH - 1; i++)
		sum += data[i];

	edid->checksum = 0x100 - (sum & 0xFF);
}

int dsi_connector_get_modes(struct drm_connector *connector, void *data)
{
	int rc, i;
	u32 count = 0, edid_size;
	struct dsi_display_mode *modes = NULL;
	struct drm_display_mode drm_mode;
	struct dsi_display *display = data;
	struct edid edid;
	const u8 edid_buf[EDID_LENGTH] = {
		0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x44, 0x6D,
		0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1B, 0x10, 0x01, 0x03,
		0x80, 0x50, 0x2D, 0x78, 0x0A, 0x0D, 0xC9, 0xA0, 0x57, 0x47,
		0x98, 0x27, 0x12, 0x48, 0x4C, 0x00, 0x00, 0x00, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01,
	};

	edid_size = min_t(u32, sizeof(edid), EDID_LENGTH);

	memcpy(&edid, edid_buf, edid_size);

	if (sde_connector_get_panel(connector)) {
		/*
		 * TODO: If drm_panel is attached, query modes from the panel.
		 * This is complicated in split dsi cases because panel is not
		 * attached to both connectors.
		 */
		goto end;
	}
	rc = dsi_display_get_mode_count(display, &count);
	if (rc) {
		pr_err("failed to get num of modes, rc=%d\n", rc);
		goto end;
	}

	rc = dsi_display_get_modes(display, &modes);
	if (rc) {
		pr_err("failed to get modes, rc=%d\n", rc);
		count = 0;
		goto end;
	}

	for (i = 0; i < count; i++) {
		struct drm_display_mode *m;

		memset(&drm_mode, 0x0, sizeof(drm_mode));
		dsi_convert_to_drm_mode(&modes[i], &drm_mode);
		m = drm_mode_duplicate(connector->dev, &drm_mode);
		if (!m) {
			pr_err("failed to add mode %ux%u\n",
			       drm_mode.hdisplay,
			       drm_mode.vdisplay);
			count = -ENOMEM;
			goto end;
		}
		m->width_mm = connector->display_info.width_mm;
		m->height_mm = connector->display_info.height_mm;
		/* set the first mode in list as preferred */
		if (i == 0)
			m->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, m);
	}

	rc = dsi_drm_update_edid_name(&edid, display->panel->name);
	if (rc) {
		count = 0;
		goto end;
	}

	dsi_drm_update_dtd(&edid, modes, count);
	dsi_drm_update_checksum(&edid);
	rc = drm_mode_connector_update_edid_property(connector, &edid);
	if (rc)
		count = 0;
end:
	pr_debug("MODE COUNT =%d\n\n", count);
	return count;
}

enum drm_mode_status dsi_conn_mode_valid(struct drm_connector *connector,
		struct drm_display_mode *mode,
		void *display)
{
	struct dsi_display_mode dsi_mode;
	int rc;

	if (!connector || !mode) {
		pr_err("Invalid params\n");
		return MODE_ERROR;
	}

	convert_to_dsi_mode(mode, &dsi_mode);

	rc = dsi_display_validate_mode(display, &dsi_mode,
			DSI_VALIDATE_FLAG_ALLOW_ADJUST);
	if (rc) {
		pr_err("mode not supported, rc=%d\n", rc);
		return MODE_BAD;
	}

	return MODE_OK;
}

struct drm_encoder *dsi_conn_atomic_best_encoder(
		struct drm_connector *connector, void *display,
		struct drm_connector_state *c_state)
{
	struct sde_connector *c_conn;

	if (!connector)
		return NULL;

	(void)display;
	(void)c_state;
	c_conn = to_sde_connector(connector);

	return c_conn->encoder;
}

int dsi_conn_atomic_check(struct drm_connector *connector, void *display,
		struct drm_connector_state *c_state)
{
	struct dsi_display *dsi_display = display;
	struct drm_crtc_state *crtc_state;
	u32 requested_rate;

	if (!connector || !dsi_display || !dsi_display->panel || !c_state ||
	    !c_state->state)
		return -EINVAL;

	if (!dsi_display->panel->step_refresh_enabled || !c_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(c_state->state,
			c_state->crtc);
	if (!crtc_state)
		return -EINVAL;

	requested_rate = dsi_bridge_mode_vrefresh(&crtc_state->mode);
	if (!crtc_state->active || !crtc_state->active_changed ||
	    requested_rate != dsi_display->panel->step_refresh_target_rate)
		return 0;

	/*
	 * An active-only resume is not considered a mode change by the DRM
	 * helper. Force a real bridge mode_set so adjusted_mode and the panel
	 * restart together from the base hardware stage.
	 */
	crtc_state->mode_changed = true;
	pr_info("[step90] force resume modeset for requested %u Hz\n",
		requested_rate);

	return 0;
}

int dsi_conn_pre_kickoff(struct drm_connector *connector,
		void *display,
		struct msm_display_kickoff_params *params)
{
	if (!connector || !display || !params) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	return dsi_display_pre_kickoff(connector, display, params);
}

int dsi_conn_prepare_commit(void *display,
		struct msm_display_conn_params *params)
{
	if (!display || !params) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	return dsi_display_pre_commit(display, params);
}

void dsi_conn_enable_event(struct drm_connector *connector,
		uint32_t event_idx, bool enable, void *display)
{
	struct dsi_event_cb_info event_info;

	memset(&event_info, 0, sizeof(event_info));

	event_info.event_cb = sde_connector_trigger_event;
	event_info.event_usr_ptr = connector;

	dsi_display_enable_event(connector, display,
			event_idx, &event_info, enable);
}

int dsi_conn_post_kickoff(struct drm_connector *connector,
	struct msm_display_conn_params *params)
{
	struct drm_encoder *encoder;
	struct dsi_bridge *c_bridge;
	struct dsi_display_mode adj_mode;
	struct dsi_display *display;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	int i, rc = 0;
	bool enable;

	if (!connector || !connector->state) {
		pr_err("invalid connector or connector state");
		return -EINVAL;
	}

	encoder = connector->state->best_encoder;
	if (!encoder) {
		pr_debug("best encoder is not available");
		return 0;
	}

	c_bridge = to_dsi_bridge(encoder->bridge);
	adj_mode = c_bridge->dsi_mode;
	display = c_bridge->display;

	if (adj_mode.dsi_mode_flags & DSI_MODE_FLAG_VRR) {
		m_ctrl = &display->ctrl[display->clk_master_idx];
		rc = dsi_ctrl_timing_db_update(m_ctrl->ctrl, false);
		if (rc) {
			pr_err("[%s] failed to dfps update  rc=%d\n",
				display->name, rc);
			return -EINVAL;
		}

		/* Update the rest of the controllers */
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl || (ctrl == m_ctrl))
				continue;

			rc = dsi_ctrl_timing_db_update(ctrl->ctrl, false);
			if (rc) {
				pr_err("[%s] failed to dfps update rc=%d\n",
					display->name,  rc);
				return -EINVAL;
			}
		}

		c_bridge->dsi_mode.dsi_mode_flags &= ~DSI_MODE_FLAG_VRR;
	}

	/* ensure dynamic clk switch flag is reset */
	c_bridge->dsi_mode.dsi_mode_flags &= ~DSI_MODE_FLAG_DYN_CLK;

	if (params->qsync_update) {
		enable = (params->qsync_mode > 0) ? true : false;
		display_for_each_ctrl(i, display) {
			dsi_ctrl_setup_avr(display->ctrl[i].ctrl, enable);
		}
	}

	dsi_bridge_schedule_step_refresh(c_bridge, connector);

	return 0;
}

struct dsi_bridge *dsi_drm_bridge_init(struct dsi_display *display,
				       struct drm_device *dev,
				       struct drm_encoder *encoder)
{
	int rc = 0;
	struct dsi_bridge *bridge;

	bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge) {
		rc = -ENOMEM;
		goto error;
	}

	bridge->display = display;
	bridge->base.funcs = &dsi_bridge_ops;
	bridge->base.encoder = encoder;
	INIT_DELAYED_WORK(&bridge->step_refresh_work,
			dsi_bridge_step_refresh_work_fn);
	spin_lock_init(&bridge->step_refresh_lock);
	bridge->step_refresh_expected_flags = 0;
	bridge->step_refresh_expected_rate = 0;
	bridge->step_refresh_generation = 0;
	bridge->step_refresh_retry_count = 0;
	atomic_set(&bridge->step_refresh_blocked, 0);
	atomic_set(&bridge->step_refresh_restart_pending, 0);
	atomic_set(&bridge->step_refresh_stage_error, 0);
	bridge->step_refresh_shutdown = false;

	rc = drm_bridge_attach(encoder, &bridge->base, NULL);
	if (rc) {
		pr_err("failed to attach bridge, rc=%d\n", rc);
		goto error_free_bridge;
	}

	encoder->bridge = &bridge->base;
	return bridge;
error_free_bridge:
	kfree(bridge);
error:
	return ERR_PTR(rc);
}

void dsi_drm_bridge_cleanup(struct dsi_bridge *bridge)
{
	unsigned long irq_flags;

	if (!bridge)
		return;

	spin_lock_irqsave(&bridge->step_refresh_lock, irq_flags);
	bridge->step_refresh_shutdown = true;
	bridge->step_refresh_expected_flags = 0;
	bridge->step_refresh_expected_rate = 0;
	bridge->step_refresh_retry_count = 0;
	bridge->step_refresh_generation++;
	atomic_set(&bridge->step_refresh_restart_pending, 0);
	atomic_set(&bridge->step_refresh_stage_error, 0);
	spin_unlock_irqrestore(&bridge->step_refresh_lock, irq_flags);
	cancel_delayed_work_sync(&bridge->step_refresh_work);

	if (bridge->base.encoder)
		bridge->base.encoder->bridge = NULL;

	kfree(bridge);
}
