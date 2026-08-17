/*
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
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

#ifndef _DSI_DRM_H_
#define _DSI_DRM_H_

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>

#include "msm_drv.h"

#include "dsi_display.h"

#define DSI_MODE_FLAG_STEP_REFRESH_NEXT_BRIDGE	BIT(16)
#define DSI_MODE_FLAG_STEP_REFRESH_NEXT_TARGET	BIT(17)
#define DSI_MODE_FLAG_STEP_REFRESH_FINAL		BIT(18)
#define DSI_MODE_FLAG_STEP_REFRESH_PENDING	\
	(DSI_MODE_FLAG_STEP_REFRESH_NEXT_BRIDGE | \
	 DSI_MODE_FLAG_STEP_REFRESH_NEXT_TARGET)
#define DSI_MODE_FLAG_STEP_REFRESH_ACTIVE	\
	(DSI_MODE_FLAG_STEP_REFRESH_PENDING | \
	 DSI_MODE_FLAG_STEP_REFRESH_FINAL)

struct dsi_bridge {
	struct drm_bridge base;
	u32 id;

	struct dsi_display *display;
	struct dsi_display_mode dsi_mode;

	struct delayed_work step_refresh_work;
	spinlock_t step_refresh_lock;
	u32 step_refresh_expected_flags;
	u32 step_refresh_expected_rate;
	u32 step_refresh_generation;
	u32 step_refresh_retry_count;
	atomic_t step_refresh_blocked;
	atomic_t step_refresh_restart_pending;
	atomic_t step_refresh_stage_error;
	bool step_refresh_shutdown;
};

void dsi_bridge_request_step_refresh_restart(struct dsi_bridge *bridge);
void dsi_bridge_invalidate_step_refresh(struct dsi_bridge *bridge);

/**
 * dsi_conn_set_info_blob - callback to perform info blob initialization
 * @connector: Pointer to drm connector structure
 * @info: Pointer to sde connector info structure
 * @display: Pointer to private display handle
 * @mode_info: Pointer to mode info structure
 * Returns: Zero on success
 */
int dsi_conn_set_info_blob(struct drm_connector *connector,
		void *info,
		void *display,
		struct msm_mode_info *mode_info);

/**
 * dsi_conn_detect - callback to determine if connector is connected
 * @connector: Pointer to drm connector structure
 * @force: Force detect setting from drm framework
 * @display: Pointer to private display handle
 * Returns: Connector 'is connected' status
 */
enum drm_connector_status dsi_conn_detect(struct drm_connector *conn,
		bool force,
		void *display);

/**
 * dsi_connector_get_modes - callback to add drm modes via drm_mode_probed_add()
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display handle
 * Returns: Number of modes added
 */
int dsi_connector_get_modes(struct drm_connector *connector,
		void *display);

/**
 * dsi_connector_put_modes - callback to free up drm modes of the connector
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display handle
 */
void dsi_connector_put_modes(struct drm_connector *connector,
	void *display);

/**
 * dsi_conn_get_mode_info - retrieve information on the mode selected
 * @drm_mode: Display mode set for the display
 * @mode_info: Out parameter. information of the mode.
 * @max_mixer_width: max width supported by HW layer mixer
 * @display: Pointer to private display structure
 * Returns: Zero on success
 */
int dsi_conn_get_mode_info(struct drm_connector *connector,
		const struct drm_display_mode *drm_mode,
		struct msm_mode_info *mode_info, u32 max_mixer_width,
		void *display);

/**
 * dsi_conn_mode_valid - callback to determine if specified mode is valid
 * @connector: Pointer to drm connector structure
 * @mode: Pointer to drm mode structure
 * @display: Pointer to private display handle
 * Returns: Validity status for specified mode
 */
enum drm_mode_status dsi_conn_mode_valid(struct drm_connector *connector,
		struct drm_display_mode *mode,
		void *display);

struct drm_encoder *dsi_conn_atomic_best_encoder(
		struct drm_connector *connector, void *display,
		struct drm_connector_state *c_state);

int dsi_conn_atomic_check(struct drm_connector *connector, void *display,
		struct drm_connector_state *c_state);

/**
 * dsi_conn_enable_event - callback to notify DSI driver of event registeration
 * @connector: Pointer to drm connector structure
 * @event_idx: Connector event index
 * @enable: Whether or not the event is enabled
 * @display: Pointer to private display handle
 */
void dsi_conn_enable_event(struct drm_connector *connector,
		uint32_t event_idx, bool enable, void *display);

struct dsi_bridge *dsi_drm_bridge_init(struct dsi_display *display,
		struct drm_device *dev,
		struct drm_encoder *encoder);

void dsi_drm_bridge_cleanup(struct dsi_bridge *bridge);

/**
 * dsi_display_pre_kickoff - program kickoff-time features
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display structure
 * @params: Parameters for kickoff-time programming
 * Returns: Zero on success
 */
int dsi_conn_pre_kickoff(struct drm_connector *connector,
		void *display,
		struct msm_display_kickoff_params *params);

/**
 * dsi_display_post_kickoff - program post kickoff-time features
 * @connector: Pointer to drm connector structure
 * @params: Parameters for post kickoff programming
 * Returns: Zero on success
 */
int dsi_conn_post_kickoff(struct drm_connector *connector,
		struct msm_display_conn_params *params);

/**
 * dsi_convert_to_drm_mode - Update drm mode with dsi mode information
 * @dsi_mode: input parameter. structure having dsi mode information.
 * @drm_mode: output parameter. DRM mode set for the display
 */
void dsi_convert_to_drm_mode(const struct dsi_display_mode *dsi_mode,
				struct drm_display_mode *drm_mode);

u64 dsi_drm_find_bit_clk_rate(void *display,
			      const struct drm_display_mode *drm_mode);

/**
 * dsi_conn_prepare_commit - program pre commit time features
 * @display: Pointer to private display structure
 * @params: Parameters for pre commit programming
 * Returns: Zero on success
 */
int dsi_conn_prepare_commit(void *display,
		struct msm_display_conn_params *params);


#endif /* _DSI_DRM_H_ */
