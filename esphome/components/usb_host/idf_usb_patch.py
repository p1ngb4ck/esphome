"""Per-port FIFO bias for the espressif/usb host component.

The DWC controller splits one FIFO between received data (RX), non-periodic OUT and
periodic OUT, and that split is what caps the largest packet each direction can carry.
_calculate_fifo_from_bias() in hcd_dwc.c already derives the split per port from that
port's own OTG_DFIFO_DEPTH, but which of the three biases it applies comes from a single
Kconfig choice that reaches every port at once.

On a chip with two host controllers the two ports want opposite things. A full-speed port
carrying an isochronous OUT stream needs the periodic-out split, and the same split on a
high-speed port leaves 256 bytes of non-periodic OUT, below the 512 byte wMaxPacketSize
that every high-speed bulk OUT endpoint has by definition, so hcd_pipe_alloc() rejects all
of them. One choice cannot serve both.

The plumbing for a per-port answer is already there: hcd_port_config_t carries a per-port
fifo_config and hcd_port_init() validates it against that port's own HAL. Only the choice
is global. These patches add a per-port bias that travels the same path as the existing
per-port fifo_config -- usb_host_config_t, hub_config_t, hcd_port_config_t -- and turn the
preprocessor chain in _calculate_fifo_from_bias() into a switch over it. A port left at
USB_HOST_FIFO_BIAS_DEFAULT resolves to the Kconfig choice, so a caller that sets nothing
sees no change.

Deliberately expressed as exact-string replacements rather than regexes: an upstream
change to any of these lines has to surface as a build-time failure naming the file, not
as a patch that silently applies to something else.
"""

from __future__ import annotations

import hashlib

# Version of the espressif/usb component these anchors were taken from. The download URL
# and the patches move together; bumping one without the other is a build failure.
USB_COMPONENT_VERSION = "1.4.1"
USB_COMPONENT_URL = (
    "https://components-file.espressif.com/components/espressif/usb/"
    f"{USB_COMPONENT_VERSION}/espressif__usb-v{USB_COMPONENT_VERSION}.zip"
)

# Bias values, mirrored in usb_host.h on the ESPHome side and in the enum below.
BIAS_DEFAULT = 0
BIAS_BALANCED = 1
BIAS_IN = 2
BIAS_PERIODIC_OUT = 3


_PUBLIC_HEADER_ANCHOR = """typedef struct {
    bool skip_phy_setup;                        /**< If set, the USB Host Library will not configure the USB PHY thus allowing"""

_PUBLIC_HEADER_ADDITION = """/**
 * @brief Hardware FIFO size biasing, per root port
 *
 * The DWC controller divides one FIFO between received data, non-periodic OUT and periodic
 * OUT. Which division is used caps the largest packet each direction can carry, so a port
 * carrying a wide isochronous OUT stream and a port carrying high-speed bulk want opposite
 * divisions. USB_HOST_FIFO_BIAS_DEFAULT leaves the port on the CONFIG_USB_HOST_HW_BUFFER_BIAS
 * choice, which is what a caller that sets nothing gets.
 */
typedef enum {
    USB_HOST_FIFO_BIAS_DEFAULT = 0, /**< Follow the CONFIG_USB_HOST_HW_BUFFER_BIAS Kconfig choice */
    USB_HOST_FIFO_BIAS_BALANCED,    /**< Balanced between the three FIFOs */
    USB_HOST_FIFO_BIAS_IN,          /**< Favour received data */
    USB_HOST_FIFO_BIAS_PERIODIC_OUT,/**< Favour periodic (isochronous, interrupt) OUT */
} usb_host_fifo_bias_t;

typedef struct {
    bool skip_phy_setup;                        /**< If set, the USB Host Library will not configure the USB PHY thus allowing"""

_PUBLIC_CONFIG_ANCHOR = (
    """    unsigned peripheral_map;      /**< Selects the USB peripheral(s) to use."""
)

_PUBLIC_CONFIG_ADDITION = """    usb_host_fifo_bias_t fifo_bias_per_port[SOC_USB_OTG_PERIPH_NUM];
                                  /**< Optional per-port FIFO size biasing. Indexed the same way as
                                       peripheral_map. An entry left at USB_HOST_FIFO_BIAS_DEFAULT
                                       follows the Kconfig choice, which is the behaviour of a
                                       caller that does not set this field at all. Unlike
                                       fifo_settings_custom this is honoured for dual port
                                       applications, since each port resolves its own split from
                                       its own FIFO depth. */
    unsigned peripheral_map;      /**< Selects the USB peripheral(s) to use."""

_PUBLIC_INCLUDE_ANCHOR = """#include "esp_intr_alloc.h"
"""

_PUBLIC_INCLUDE_ADDITION = """#include "esp_intr_alloc.h"
#include "soc/soc_caps.h"
"""

_HCD_HEADER_ANCHOR = """typedef struct {
    hcd_port_callback_t callback;           /**< HCD port event callback */
    void *callback_arg;                     /**< User argument for HCD port callback */
    void *context;                          /**< Context variable used to associate the port with upper layer object */
    const hcd_fifo_settings_t *fifo_config; /**< Optional pointer to custom FIFO config. If NULL, default configuration is used. */
    int intr_flags;                         /**< Interrupt flags for HCD interrupt */
} hcd_port_config_t;"""

_HCD_HEADER_REPLACEMENT = """/**
 * @brief Which of the hardware FIFO size biases a port uses
 *
 * Kept numerically identical to usb_host_fifo_bias_t so the value can be passed straight
 * through without a translation table that could drift.
 */
typedef enum {
    HCD_FIFO_BIAS_DEFAULT = 0,   /**< Follow the CONFIG_USB_HOST_HW_BUFFER_BIAS Kconfig choice */
    HCD_FIFO_BIAS_BALANCED,      /**< Balanced between the three FIFOs */
    HCD_FIFO_BIAS_IN,            /**< Favour received data */
    HCD_FIFO_BIAS_PERIODIC_OUT,  /**< Favour periodic (isochronous, interrupt) OUT */
} hcd_fifo_bias_t;

typedef struct {
    hcd_port_callback_t callback;           /**< HCD port event callback */
    void *callback_arg;                     /**< User argument for HCD port callback */
    void *context;                          /**< Context variable used to associate the port with upper layer object */
    const hcd_fifo_settings_t *fifo_config; /**< Optional pointer to custom FIFO config. If NULL, default configuration is used. */
    hcd_fifo_bias_t fifo_bias;              /**< Bias used when fifo_config is NULL. HCD_FIFO_BIAS_DEFAULT follows Kconfig. */
    int intr_flags;                         /**< Interrupt flags for HCD interrupt */
} hcd_port_config_t;"""

_HCD_CALC_ANCHOR = """static void  _calculate_fifo_from_bias(port_t *port, const usb_dwc_hal_context_t *hal)
{
    const int otg_dfifo_depth = hal->constant_config.hsphy_type ? 1024 : 256;
    const uint16_t fifo_size_lines = hal->constant_config.fifo_size;

#if CONFIG_USB_HOST_HW_BUFFER_BIAS_IN
    // Prioritize RX FIFO (best for IN-heavy workloads)
    port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 16;
    port->fifo_config.ptx_fifo_lines = otg_dfifo_depth / 8;
    port->fifo_config.rx_fifo_lines = fifo_size_lines - port->fifo_config.ptx_fifo_lines - port->fifo_config.nptx_fifo_lines;

#elif CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT
    // Prioritize periodic TX FIFO (useful for high throughput periodic endpoints)
    port->fifo_config.rx_fifo_lines = otg_dfifo_depth / 8 + 2; // 2 extra lines are allocated for status information. See USB-OTG Programming Guide, chapter 2.1.2.1
    port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 16;
    port->fifo_config.ptx_fifo_lines = fifo_size_lines - port->fifo_config.nptx_fifo_lines - port->fifo_config.rx_fifo_lines;

#else // USB_HOST_HW_BUFFER_BIAS_BALANCED
    // Balanced configuration (default)
    port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 4;
    port->fifo_config.ptx_fifo_lines = otg_dfifo_depth / 8;
    port->fifo_config.rx_fifo_lines = fifo_size_lines - port->fifo_config.ptx_fifo_lines - port->fifo_config.nptx_fifo_lines;
#endif
}"""

_HCD_CALC_REPLACEMENT = """static hcd_fifo_bias_t _kconfig_fifo_bias(void)
{
#if CONFIG_USB_HOST_HW_BUFFER_BIAS_IN
    return HCD_FIFO_BIAS_IN;
#elif CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT
    return HCD_FIFO_BIAS_PERIODIC_OUT;
#else
    return HCD_FIFO_BIAS_BALANCED;
#endif
}

static void  _calculate_fifo_from_bias(port_t *port, const usb_dwc_hal_context_t *hal, hcd_fifo_bias_t bias)
{
    const int otg_dfifo_depth = hal->constant_config.hsphy_type ? 1024 : 256;
    const uint16_t fifo_size_lines = hal->constant_config.fifo_size;

    if (bias == HCD_FIFO_BIAS_DEFAULT) {
        bias = _kconfig_fifo_bias();
    }

    switch (bias) {
    case HCD_FIFO_BIAS_IN:
        // Prioritize RX FIFO (best for IN-heavy workloads)
        port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 16;
        port->fifo_config.ptx_fifo_lines = otg_dfifo_depth / 8;
        port->fifo_config.rx_fifo_lines = fifo_size_lines - port->fifo_config.ptx_fifo_lines - port->fifo_config.nptx_fifo_lines;
        break;

    case HCD_FIFO_BIAS_PERIODIC_OUT:
        // Prioritize periodic TX FIFO (useful for high throughput periodic endpoints)
        port->fifo_config.rx_fifo_lines = otg_dfifo_depth / 8 + 2; // 2 extra lines are allocated for status information. See USB-OTG Programming Guide, chapter 2.1.2.1
        port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 16;
        port->fifo_config.ptx_fifo_lines = fifo_size_lines - port->fifo_config.nptx_fifo_lines - port->fifo_config.rx_fifo_lines;
        break;

    default:
        // Balanced configuration (default)
        port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 4;
        port->fifo_config.ptx_fifo_lines = otg_dfifo_depth / 8;
        port->fifo_config.rx_fifo_lines = fifo_size_lines - port->fifo_config.ptx_fifo_lines - port->fifo_config.nptx_fifo_lines;
        break;
    }
}"""

_HCD_CALL_ANCHOR = """        _calculate_fifo_from_bias(port_obj, port_obj->hal);"""

_HCD_CALL_REPLACEMENT = """        _calculate_fifo_from_bias(port_obj, port_obj->hal, port_config->fifo_bias);"""

_HUB_HEADER_ANCHOR = """    const hcd_fifo_settings_t *fifo_config;         /**< Optional pointer to custom FIFO config. If NULL, default configuration is used. */"""

_HUB_HEADER_REPLACEMENT = """    const hcd_fifo_settings_t *fifo_config;         /**< Optional pointer to custom FIFO config. If NULL, default configuration is used. */
    hcd_fifo_bias_t fifo_bias[HCD_NUM_PORTS];       /**< Per-port bias used where fifo_config is NULL. */"""

_HUB_PORT_ANCHOR = """                .intr_flags = hub_config->intr_flags,
                .fifo_config = hub_config->fifo_config,"""

_HUB_PORT_REPLACEMENT = """                .intr_flags = hub_config->intr_flags,
                .fifo_config = hub_config->fifo_config,
                .fifo_bias = hub_config->fifo_bias[i],"""

_HOST_WARN_ANCHOR = """        .intr_flags = config->intr_flags,
        .fifo_config = NULL,
    };"""

_HOST_WARN_REPLACEMENT = """        .intr_flags = config->intr_flags,
        .fifo_config = NULL,
    };

    // Per-port FIFO biasing. Unlike fifo_settings_custom this is meaningful with more than
    // one port: the bias only selects which of the three splits a port uses, and each port
    // resolves that split against its own FIFO depth in _calculate_fifo_from_bias().
    for (int i = 0; i < SOC_USB_OTG_PERIPH_NUM; i++) {
        hub_config.fifo_bias[i] = (hcd_fifo_bias_t)config->fifo_bias_per_port[i];
    }"""

_RESET_BAILOUT_ANCHOR = """    ret = ESP_OK;
bailout:
    _port_reset_all_pipes(port);
exit:
    return ret;
}"""

_RESET_BAILOUT_REPLACEMENT = """    ret = ESP_OK;
bailout:
    // A reset that gets this far has already moved the port to RESETTING, and no command
    // accepts that state: leaving it there strands the port until the device is unplugged.
    // Worse, root_port_recycle() in the Hub driver switches on the port state with an
    // abort() default, and RESETTING lands there -- so a failed reset during enumeration,
    // where hub_node_reset() issues a second one, took the firmware down when the device was
    // afterwards torn down.
    //
    // Only claim the state when it is still RESETTING. Anything else means the interrupt
    // handler already moved the port on, and it left a port event pending that the Hub
    // driver is about to consume; overwriting that would lose the disconnection.
    if (port->state == HCD_PORT_STATE_RESETTING) {
        port->flags.conn_dev_ena = 0;
        port->state = HCD_PORT_STATE_RECOVERY;
    }
    _port_reset_all_pipes(port);
exit:
    return ret;
}"""

_HUB_RESET_ERR_ANCHOR = """new_dev_err:
        // We allow this to fail in case a disconnect/port error happens while disabling.
        hcd_port_command(root_port_hdl, HCD_PORT_CMD_DISABLE);
reset_err:
        break;
    }"""

_HUB_RESET_ERR_REPLACEMENT = """new_dev_err:
        // We allow this to fail in case a disconnect/port error happens while disabling.
        hcd_port_command(root_port_hdl, HCD_PORT_CMD_DISABLE);
reset_err:
        // A failed reset leaves nothing behind to drive the port forward: no device object
        // exists yet, so root_port_recycle() will never run for it. Ask for recovery here,
        // but only where the port is actually recoverable -- _port_cmd_reset() moves the
        // port to RECOVERY when its own attempt failed, and the interrupt handler does the
        // same on disconnection, error and overcurrent.
        //
        // The request is safe to raise from inside the event handler even when a
        // disconnection arrived during the reset. hub_process() takes its action snapshot
        // before calling this function, so the request bit set here is only acted on in its
        // next iteration, and that iteration handles the port event before the port request.
        // hcd_port_handle_event() has therefore cleared flags.event_pending by the time
        // hcd_port_recover() checks that every port flag is clear.
        if (hcd_port_get_state(root_port_hdl) == HCD_PORT_STATE_RECOVERY) {
            HUB_DRIVER_ENTER_CRITICAL();
            root_hub_port->dynamic.reqs |= PORT_REQ_RECOVER;
            if (root_hub_port->constant.index == 0) {
                p_hub_driver_obj->dynamic.flags.actions |= HUB_DRIVER_ACTION_ROOT0_REQ;
            } else {
#if HCD_NUM_PORTS > 1
                p_hub_driver_obj->dynamic.flags.actions |= HUB_DRIVER_ACTION_ROOT1_REQ;
#else
                abort();    // Should never occur
#endif // HCD_NUM_PORTS > 1
            }
            HUB_DRIVER_EXIT_CRITICAL();
        }
        break;
    }"""

# The high-speed controller as a full/low-speed host: HCFG.FSLSSupp set before every port
# reset, so the controller does not answer a device's high-speed chirp. Backported from
# espressif/usb 1.6.0 (usb_host_config_t.fsls_only), which needs it from an ESP-IDF HAL that
# neither 5.5 nor 6.1 has; the LL setter it ends in is in both, so it is called directly.
_FSLS_PUBLIC_ANCHOR = (
    """    unsigned peripheral_map;      /**< Selects the USB peripheral(s) to use."""
)

_FSLS_PUBLIC_REPLACEMENT = """    bool fsls_only;               /**< If set, High-Speed capable USB-OTG peripherals operate as Full/Low-Speed only
                                       hosts. The host will not respond to a connected device's High-Speed chirp.
                                       Has no effect on Full-Speed only peripherals. */
    unsigned peripheral_map;      /**< Selects the USB peripheral(s) to use."""

_FSLS_HCD_HEADER_ANCHOR = """    hcd_fifo_bias_t fifo_bias;              /**< Bias used when fifo_config is NULL. HCD_FIFO_BIAS_DEFAULT follows Kconfig. */"""

_FSLS_HCD_HEADER_REPLACEMENT = """    hcd_fifo_bias_t fifo_bias;              /**< Bias used when fifo_config is NULL. HCD_FIFO_BIAS_DEFAULT follows Kconfig. */
    bool fsls_only;                         /**< Operate a High-Speed capable port as Full/Low-Speed only. */"""

_FSLS_HUB_HEADER_ANCHOR = """    hcd_fifo_bias_t fifo_bias[HCD_NUM_PORTS];       /**< Per-port bias used where fifo_config is NULL. */"""

_FSLS_HUB_HEADER_REPLACEMENT = """    hcd_fifo_bias_t fifo_bias[HCD_NUM_PORTS];       /**< Per-port bias used where fifo_config is NULL. */
    bool fsls_only;                                 /**< Operate High-Speed root ports as Full/Low-Speed only. */"""

_FSLS_HUB_PORT_ANCHOR = """                .fifo_bias = hub_config->fifo_bias[i],"""

_FSLS_HUB_PORT_REPLACEMENT = """                .fifo_bias = hub_config->fifo_bias[i],
                .fsls_only = hub_config->fsls_only,"""

_FSLS_HOST_ANCHOR = """        .intr_flags = config->intr_flags,
        .fifo_config = NULL,
    };"""

_FSLS_HOST_REPLACEMENT = """        .intr_flags = config->intr_flags,
        .fifo_config = NULL,
        .fsls_only = config->fsls_only,
    };"""

_FSLS_PORT_FIELD_ANCHOR = """    int periph_idx;                                 // Peripheral index of this port. Used for initialization check"""

_FSLS_PORT_FIELD_REPLACEMENT = """    int periph_idx;                                 // Peripheral index of this port. Used for initialization check
    bool fsls_only;                                 // High-Speed capable port held to Full/Low-Speed"""

_FSLS_PORT_INIT_ANCHOR = """    usb_dwc_hal_init(port_obj->hal, port_number);"""

_FSLS_PORT_INIT_REPLACEMENT = """    usb_dwc_hal_init(port_obj->hal, port_number);
    port_obj->fsls_only = port_config->fsls_only && port_obj->hal->constant_config.hsphy_type != 0;"""

_FSLS_RESET_ANCHOR = """    // Place the bus into the reset state. If the port was previously enabled, a disabled event will occur after this
    usb_dwc_hal_port_toggle_reset(port->hal, true);"""

_FSLS_RESET_REPLACEMENT = """    // Place the bus into the reset state. If the port was previously enabled, a disabled event will occur after this
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    if (port->fsls_only) {
        usb_dwc_ll_hcfg_set_fsls_supp_only(port->hal->dev);
    }
#endif
    usb_dwc_hal_port_toggle_reset(port->hal, true);"""

# External hub ports. A hub has a single control transfer, shared by all of its ports, and the
# port driver handles one port at a time for that reason. Three paths let a port leave or redo
# its handling while one of its requests was still in flight, so the next request on that hub
# failed to submit (ESP_ERR_INVALID_STATE) and the whole hub was dropped:
# - port_active() completed the port no matter what it was doing;
# - a recycle submitted the port's next request while the last one was still running;
# - a status read was submitted while a request was in flight.
# A request in flight is tracked per port, a port finishes or recycles once it is done, and a
# status read waits for it (the completion reads the status again). With that, a status read of
# an enumerated port must not be reported as a reset completion, so a reset in progress is
# tracked too. Ports of a hub that is being released are not handled any more: a late completion
# would put them back into the pending list, and finishing them asks their hub, which may be
# freed by then.
_PORT_FLAGS_ANCHOR = """            uint32_t waiting_free: 1;       /**< Port is waiting to be freed */
            uint32_t reserved25: 25;        /**< Reserved */"""

_PORT_FLAGS_REPLACEMENT = """            uint32_t waiting_free: 1;       /**< Port is waiting to be freed */
            uint32_t req_in_flight: 1;      /**< A request to the parent Hub (status or feature) has not completed yet */
            uint32_t reset_pending: 1;      /**< A port reset was issued and its completion has not been reported yet */
            uint32_t recycle_deferred: 1;   /**< A recycle waits for the request in flight to complete */
            uint32_t reserved22: 22;        /**< Reserved */"""

_PORT_STATUS_REQ_ANCHOR = """    // Port is requesting status, lock the status
    ext_port->flags.status_lock = 1;
    return ESP_OK;"""

_PORT_STATUS_REQ_REPLACEMENT = """    // Port is requesting status, lock the status
    ext_port->flags.status_lock = 1;
    ext_port->flags.req_in_flight = 1;
    return ESP_OK;"""

_PORT_SET_FEATURE_ANCHOR = """    // Every set feature requires status update
    ext_port->flags.status_outdated = 1;
    switch (feature) {"""

_PORT_SET_FEATURE_REPLACEMENT = """    // Every set feature requires status update
    ext_port->flags.status_outdated = 1;
    ext_port->flags.req_in_flight = 1;
    if (feature == USB_FEATURE_PORT_RESET) {
        ext_port->flags.reset_pending = 1;
    }
    switch (feature) {"""

_PORT_CLEAR_FEATURE_ANCHOR = """    // Every clearing feature requires status update
    ext_port->flags.status_outdated = 1;
    return ret;"""

_PORT_CLEAR_FEATURE_REPLACEMENT = """    // Every clearing feature requires status update
    ext_port->flags.status_outdated = 1;
    ext_port->flags.req_in_flight = 1;
    return ret;"""

_PORT_COMPLETE_ANCHOR = """             (ext_port->dev_state == PORT_DEV_PRESENT) ? "active" : "idle");

    assert(ext_port->flags.is_gone == 0);"""

_PORT_COMPLETE_REPLACEMENT = """             (ext_port->dev_state == PORT_DEV_PRESENT) ? "active" : "idle");

    if (ext_port->flags.is_gone) {
        // The parent Hub is being released and may already be freed: nothing to tell it
        port_stop_handling(ext_port);
        return ESP_OK;
    }

    assert(ext_port->flags.is_gone == 0);"""

_PORT_RESETTING_ANCHOR = """                assert(port_is_enabled(ext_port)); // Port should be enabled
                port_event(ext_port, EXT_PORT_RESET_COMPLETED);"""

_PORT_RESETTING_REPLACEMENT = """                assert(port_is_enabled(ext_port)); // Port should be enabled
                ext_port->flags.reset_pending = 0;
                port_event(ext_port, EXT_PORT_RESET_COMPLETED);"""

_PORT_ENABLED_ANCHOR = """                    ext_port->dev_state = PORT_DEV_PRESENT;
                    port_event(ext_port, EXT_PORT_CONNECTED);
                } else {
                    // Port enabled, device present, reset completed
                    ext_port->dev_reset_attempts = 0;
                    port_event(ext_port, EXT_PORT_RESET_COMPLETED);
                }
                need_handling = true;"""

_PORT_ENABLED_REPLACEMENT = """                    ext_port->flags.reset_pending = 0;
                    ext_port->dev_state = PORT_DEV_PRESENT;
                    port_event(ext_port, EXT_PORT_CONNECTED);
                    need_handling = true;
                } else if (ext_port->flags.reset_pending) {
                    // Port enabled, device present, reset completed
                    ext_port->flags.reset_pending = 0;
                    ext_port->dev_reset_attempts = 0;
                    port_event(ext_port, EXT_PORT_RESET_COMPLETED);
                    need_handling = true;
                } else if (!ext_port->flags.has_enum_device) {
                    // A status read during the enumeration: the port finishes in port_active()
                    need_handling = true;
                }
                // Otherwise the device is enumerated and nothing changed: the port is done"""

_PORT_RECYCLE_ANCHOR = """    ext_port->flags.waiting_recycle = 0;

    if (ext_port->flags.status_lock) {
        // Port is awaiting the status
        return;
    }

    // Port should not have any changes
    assert(ext_port->status.wPortChange.val == 0);
    switch (ext_port->state) {"""

_PORT_RECYCLE_REPLACEMENT = """    if (ext_port->flags.req_in_flight || ext_port->flags.status_outdated) {
        // A request is in flight, or the status it outdated is still to be read: recycle from the
        // status that comes next (port_set_status() / port_req_process())
        ext_port->flags.recycle_deferred = 1;
        return;
    }

    ext_port->flags.waiting_recycle = 0;

    if (ext_port->status.wPortChange.val != 0) {
        // The port changed again while its device was being freed: handle the changes from a
        // disconnected port instead of recycling from a stale state
        if (ext_port->state != USB_PORT_STATE_DISABLED) {
            ext_port->state = USB_PORT_STATE_DISCONNECTED;
        }
        if (ext_port->flags.is_gone) {
            handle_complete(ext_port);
        } else {
            handle_port(ext_port);
        }
        return;
    }
    switch (ext_port->state) {"""

_PORT_ACTIVE_ANCHOR = """    ext_port->flags.has_enum_device = 1;

    return handle_complete(ext_port);"""

_PORT_ACTIVE_REPLACEMENT = """    ext_port->flags.has_enum_device = 1;

    if (!ext_port->flags.in_pending_list || ext_port->flags.waiting_recycle) {
        // Port is not waiting for its device to become active
        return ESP_OK;
    }
    if (ext_port->flags.req_in_flight || ext_port->flags.status_outdated || ext_port->action_flags != 0) {
        // Still busy with the parent Hub: the port finishes once that is done (handle_port_state())
        return ESP_OK;
    }
    return handle_complete(ext_port);"""

_PORT_GET_STATUS_ANCHOR = """    ext_port_t *ext_port = (ext_port_t *)port_hdl;

    port_set_actions(ext_port, PORT_ACTION_GET_STATUS);
    return ESP_OK;"""

_PORT_GET_STATUS_REPLACEMENT = """    ext_port_t *ext_port = (ext_port_t *)port_hdl;

    if (ext_port->flags.is_gone) {
        return ESP_OK;
    }
    port_set_actions(ext_port, PORT_ACTION_GET_STATUS);
    return ESP_OK;"""

_PORT_SET_STATUS_ANCHOR = """    // Remove status lock
    ext_port->flags.status_lock = 0;
    // Request port handling
    port_set_actions(ext_port, PORT_ACTION_HANDLE);
    return ESP_OK;"""

_PORT_SET_STATUS_REPLACEMENT = """    // Remove status lock
    ext_port->flags.status_lock = 0;
    ext_port->flags.req_in_flight = 0;
    if (ext_port->flags.is_gone) {
        return ESP_OK;
    }
    if (ext_port->flags.recycle_deferred) {
        ext_port->flags.recycle_deferred = 0;
        port_set_actions(ext_port, PORT_ACTION_RECYCLE);
        return ESP_OK;
    }
    // Request port handling
    port_set_actions(ext_port, PORT_ACTION_HANDLE);
    return ESP_OK;"""

_PORT_REQ_PROCESS_ANCHOR = """    ext_port_t *ext_port = (ext_port_t *)port_hdl;

    if (ext_port->flags.status_outdated) {
        port_set_actions(ext_port, PORT_ACTION_GET_STATUS);
    } else {
        port_set_actions(ext_port, PORT_ACTION_HANDLE);
    }"""

_PORT_REQ_PROCESS_REPLACEMENT = """    ext_port_t *ext_port = (ext_port_t *)port_hdl;

    ext_port->flags.req_in_flight = 0;
    if (ext_port->flags.is_gone) {
        return ESP_OK;
    }
    if (ext_port->flags.status_outdated) {
        port_set_actions(ext_port, PORT_ACTION_GET_STATUS);
    } else if (ext_port->flags.recycle_deferred) {
        ext_port->flags.recycle_deferred = 0;
        port_set_actions(ext_port, PORT_ACTION_RECYCLE);
    } else {
        port_set_actions(ext_port, PORT_ACTION_HANDLE);
    }"""

_PORT_PROCESS_STATUS_ANCHOR = """        if (action_flags & PORT_ACTION_GET_STATUS) {
            port_request_status(ext_port);
        } else if (action_flags & PORT_ACTION_RESET) {"""

_PORT_PROCESS_STATUS_REPLACEMENT = """        if (action_flags & PORT_ACTION_GET_STATUS) {
            // With a request in flight its completion reads the status again
            if (!ext_port->flags.req_in_flight) {
                port_request_status(ext_port);
            }
        } else if (action_flags & PORT_ACTION_RESET) {"""

_HUB_PORT_FEATURE_ANCHOR = """    // [TODO: IDF-12174] Revisit the External Hub Driver to ensure consistent error handling.
    ESP_ERROR_CHECK(p_ext_hub_driver->constant.port_driver->req_process(ext_hub_dev->constant.ports[port_idx]));"""

_HUB_PORT_FEATURE_REPLACEMENT = """    if (ext_hub_dev->constant.ports[port_idx] == NULL) {
        return;  // Port freed while its request was in flight (Hub released)
    }
    // [TODO: IDF-12174] Revisit the External Hub Driver to ensure consistent error handling.
    ESP_ERROR_CHECK(p_ext_hub_driver->constant.port_driver->req_process(ext_hub_dev->constant.ports[port_idx]));"""

_HUB_PORT_STATUS_ANCHOR = """    // [TODO: IDF-12174] Revisit the External Hub Driver to ensure consistent error handling.
    ESP_ERROR_CHECK(p_ext_hub_driver->constant.port_driver->set_status(ext_hub_dev->constant.ports[port_idx], new_status));"""

_HUB_PORT_STATUS_REPLACEMENT = """    if (ext_hub_dev->constant.ports[port_idx] == NULL) {
        return;  // Port freed while its request was in flight (Hub released)
    }
    // [TODO: IDF-12174] Revisit the External Hub Driver to ensure consistent error handling.
    ESP_ERROR_CHECK(p_ext_hub_driver->constant.port_driver->set_status(ext_hub_dev->constant.ports[port_idx], new_status));"""

_HUB_REQUEST_ANCHOR = """    switch (data->type) {
    case EXT_PORT_PARENT_REQ_CONTROL:
        return ext_hub_control_request(ext_hub_dev, port1, data->control.req, data->control.feature);"""

_HUB_REQUEST_REPLACEMENT = """    EXT_HUB_ENTER_CRITICAL();
    const bool releasing = ext_hub_dev->dynamic.flags.waiting_release;
    EXT_HUB_EXIT_CRITICAL();

    switch (data->type) {
    case EXT_PORT_PARENT_REQ_CONTROL:
        if (releasing) {
            // Hub already failed and is being released: its ports follow it
            return ESP_ERR_INVALID_STATE;
        }
        return ext_hub_control_request(ext_hub_dev, port1, data->control.req, data->control.feature);"""

# A status change report with the hub's own bit and port bits: device_has_changed() re-armed the
# IN endpoint at once, and the last handled port re-armed it again (PROC_COMPLETED), which fails
# with the URB still in flight and drops the hub. Re-armed right away only when no port changed.
_HUB_STATUS_CHANGE_ANCHOR = """        if (device_status & EXT_HUB_STATUS_CHANGE_FLAG) {
            // Check device status
            device_has_changed(ext_hub_dev);
        }"""

_HUB_STATUS_CHANGE_REPLACEMENT = """        if (device_status == EXT_HUB_STATUS_CHANGE_FLAG) {
            // Check device status
            device_has_changed(ext_hub_dev);
        } else if (device_status & EXT_HUB_STATUS_CHANGE_FLAG) {
            // The IN EP is re-armed once the changed ports were handled
            ESP_LOGW(EXT_HUB_TAG, "Hub status change has not been implemented yet");
        }"""

# A pipe halt that ends in a channel error. _pipe_cmd_halt() asks the channel to halt and waits
# for the HALT_REQ event, but a channel that stops on an error at the same time reports ERROR
# (the HAL ranks errors first) and nobody wakes the waiter: the task halting the pipe waits for
# good. That task is the one releasing a device that is gone (usbh, or a class driver halting
# its IN endpoints on the main loop). The error now ends the requested halt as HALT_REQ would,
# and clears the HAL's request flag (left set, the next halt of that channel reads as one more
# request). waiting_halt was never cleared after HALT_REQ either.
_HALT_ON_ERROR_ANCHOR = """        usb_dwc_hal_chan_error_t chan_error = usb_dwc_hal_chan_get_error(chan_obj);
        pipe->last_event = pipe_decode_error_event(chan_error);"""

_HALT_ON_ERROR_REPLACEMENT = """        usb_dwc_hal_chan_error_t chan_error = usb_dwc_hal_chan_get_error(chan_obj);
        if (pipe->cs_flags.waiting_halt) {
            chan_obj->flags.halt_requested = 0;
            pipe->cs_flags.waiting_halt = 0;
            pipe->last_event = HCD_PIPE_EVENT_URB_DONE;
            event = pipe->last_event;
            pipe->state = HCD_PIPE_STATE_HALTED;
            _buffer_done(pipe, usb_dwc_hal_chan_get_qtd_idx(chan_obj), HCD_PIPE_EVENT_NONE, true);
            _buffer_parse(pipe);
            *yield |= _internal_pipe_event_notify(pipe, true);
            break;
        }
        pipe->last_event = pipe_decode_error_event(chan_error);"""

_HALT_DONE_ANCHOR = """        // Notify the task waiting for the pipe halt
        *yield |= _internal_pipe_event_notify(pipe, true);"""

_HALT_DONE_REPLACEMENT = """        // Notify the task waiting for the pipe halt
        pipe->cs_flags.waiting_halt = 0;
        *yield |= _internal_pipe_event_notify(pipe, true);"""

# Every port of a hub is powered before a device on one of them is reset and enumerated. The
# ports are handled one at a time, each to its end, so a later port was powered only once the
# device on an earlier one was enumerated: right when its class driver starts talking to it,
# with the port's power switched on next to it. A port with a device now goes back to the end
# of the list while a port of its hub still waits for power, and takes its turn again with a
# fresh status. ext_port_process() leaves the actions of a port that moved back for its turn.
_PORT_POWER_FIRST_ANCHOR = """    if (handle_port_changes(ext_port)) {
        return;
    } else if (handle_port_status(ext_port)) {
        return;
    }
    handle_port_state(ext_port);
}"""

_PORT_POWER_FIRST_REPLACEMENT = """    if (handle_port_changes(ext_port)) {
        return;
    } else if (handle_port_status(ext_port)) {
        return;
    }
    if ((ext_port->state == USB_PORT_STATE_DISCONNECTED || ext_port->state == USB_PORT_STATE_RESETTING) &&
            port_has_connection(ext_port) && ext_port->dev_state == PORT_DEV_NOT_PRESENT &&
            !ext_port->flags.reset_pending) {
        ext_port_t *port;
        TAILQ_FOREACH(port, &p_ext_port_driver->single_thread.pending_tailq, tailq_entry) {
            if (port != ext_port && port->constant.context == ext_port->constant.context &&
                    (port->state == USB_PORT_STATE_NOT_CONFIGURED || port->state == USB_PORT_STATE_POWERED_OFF)) {
                break;
            }
        }
        if (port != NULL) {
            ESP_LOGD(EXT_PORT_TAG, "Port%d waits for port%d to be powered", ext_port->constant.port_num,
                     port->constant.port_num);
            TAILQ_REMOVE(&p_ext_port_driver->single_thread.pending_tailq, ext_port, tailq_entry);
            TAILQ_INSERT_TAIL(&p_ext_port_driver->single_thread.pending_tailq, ext_port, tailq_entry);
            port_set_actions(ext_port, PORT_ACTION_GET_STATUS);
            return;
        }
    }
    handle_port_state(ext_port);
}"""

_PORT_POWER_FIRST_LOOP_ANCHOR = """        action_flags = ext_port->action_flags;
        ext_port->action_flags = 0;
    }"""

_PORT_POWER_FIRST_LOOP_REPLACEMENT = """        if (ext_port->flags.in_pending_list &&
                TAILQ_FIRST(&p_ext_port_driver->single_thread.pending_tailq) != ext_port) {
            // Moved back in the list: its actions wait for its turn
            break;
        }
        action_flags = ext_port->action_flags;
        ext_port->action_flags = 0;
    }"""

# A device takes its new address only once the status stage of SET_ADDRESS is done on its side;
# a low-speed device behind a full-speed hub that misses the host's last handshake there stays at
# address 0, while the host goes on at the new one and gets no answer. When the full device
# descriptor gets no answer at the new address, the same request goes once to address 0; if the
# device answers there, SET_ADDRESS runs again and enumeration goes on. If not, it fails as before.
# addr_probe: 0 not used, 1 next request to address 0, 2 that request in flight, 3 used.
_ENUM_PROBE_FIELD_ANCHOR = """    uint8_t bConfigurationValue;    /**< Device's current configuration number */"""

_ENUM_PROBE_FIELD_REPLACEMENT = """    uint8_t bConfigurationValue;    /**< Device's current configuration number */
    uint8_t addr_probe;             /**< Device descriptor asked at address 0 after no answer at the new one */"""

_ENUM_PROBE_STATUS_ANCHOR = """    if (ctrl_xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (ctrl_xfer->status == USB_TRANSFER_STATUS_STALL &&"""

_ENUM_PROBE_STATUS_REPLACEMENT = """    if (ctrl_xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (stage == ENUM_STAGE_CHECK_FULL_DEV_DESC && ctrl_xfer->status == USB_TRANSFER_STATUS_ERROR &&
                p_enum_driver->single_thread.dev_params.addr_probe == 0 &&
                usbh_dev_clear_addr(p_enum_driver->single_thread.dev_hdl) == ESP_OK) {
            ESP_LOGW(ENUM_TAG, "No answer at address %d, asking address 0",
                     p_enum_driver->single_thread.dev_params.new_dev_addr);
            p_enum_driver->single_thread.dev_params.addr_probe = 1;
            return ret;
        }
        if (ctrl_xfer->status == USB_TRANSFER_STATUS_STALL &&"""

_ENUM_PROBE_PARSE_ANCHOR = """    case ENUM_STAGE_CHECK_FULL_DEV_DESC:
        ret = parse_full_dev_desc();"""

_ENUM_PROBE_PARSE_REPLACEMENT = """    case ENUM_STAGE_CHECK_FULL_DEV_DESC:
        // An answer at address 0 is only the sign to set the address again
        ret = (p_enum_driver->single_thread.dev_params.addr_probe == 2) ? ESP_OK : parse_full_dev_desc();"""

_ENUM_PROBE_NEXT_ANCHOR = """            } else {
                // Simply increment to get the next stage
                next_stage = last_stage + 1;
            }"""

_ENUM_PROBE_NEXT_REPLACEMENT = """            } else if (last_stage == ENUM_STAGE_CHECK_FULL_DEV_DESC &&
                       p_enum_driver->single_thread.dev_params.addr_probe == 2) {
                // The device answered at address 0: set its address again
                ESP_LOGW(ENUM_TAG, "Device still at address 0, setting its address again");
                p_enum_driver->single_thread.dev_params.addr_probe = 3;
                next_stage = ENUM_STAGE_SET_ADDR;
            } else {
                // Simply increment to get the next stage
                next_stage = last_stage + 1;
            }"""

_ENUM_PROBE_FAIL_ANCHOR = """            default:
                // Stage is not allowed to failed. Cancel enumeration."""

_ENUM_PROBE_FAIL_REPLACEMENT = """            case ENUM_STAGE_CHECK_FULL_DEV_DESC:
                if (p_enum_driver->single_thread.dev_params.addr_probe == 1) {
                    p_enum_driver->single_thread.dev_params.addr_probe = 2;
                    next_stage = ENUM_STAGE_GET_FULL_DEV_DESC;
                    break;
                }
                ESP_LOGE(ENUM_TAG, "%s FAILED", enum_stage_strings[last_stage]);
                next_stage = ENUM_STAGE_CANCEL;
                break;
            default:
                // Stage is not allowed to failed. Cancel enumeration."""

_USBH_CLEAR_ADDR_ANCHOR = """esp_err_t usbh_dev_set_desc(usb_device_handle_t dev_hdl, const usb_device_desc_t *device_desc)
{"""

_USBH_CLEAR_ADDR_REPLACEMENT = """esp_err_t usbh_dev_clear_addr(usb_device_handle_t dev_hdl)
{
    USBH_CHECK(dev_hdl != NULL, ESP_ERR_INVALID_ARG);
    esp_err_t ret;
    device_t *dev_obj = (device_t *)dev_hdl;

    USBH_ENTER_CRITICAL();
    USBH_CHECK_FROM_CRIT(dev_obj->dynamic.state == USB_DEVICE_STATE_ADDRESS, ESP_ERR_INVALID_STATE);
    USBH_CHECK_FROM_CRIT(dev_obj->dynamic.flags.enum_lock, ESP_ERR_NOT_ALLOWED);
    ret = hcd_pipe_update_dev_addr(dev_obj->constant.default_pipe, 0);
    if (ret == ESP_OK) {
        dev_obj->constant.address = 0;
        dev_obj->dynamic.state = USB_DEVICE_STATE_DEFAULT;
    }
    USBH_EXIT_CRITICAL();

    return ret;
}

esp_err_t usbh_dev_set_desc(usb_device_handle_t dev_hdl, const usb_device_desc_t *device_desc)
{"""

_USBH_CLEAR_ADDR_HEADER_ANCHOR = """esp_err_t usbh_dev_set_addr(usb_device_handle_t dev_hdl, uint8_t dev_addr);"""

_USBH_CLEAR_ADDR_HEADER_REPLACEMENT = """esp_err_t usbh_dev_set_addr(usb_device_handle_t dev_hdl, uint8_t dev_addr);

/**
 * @brief Point a device and its default pipe back at address 0 (the device kept it)
 *
 * The device must be in the address state with its enumeration lock set; it is in the default
 * state after.
 */
esp_err_t usbh_dev_clear_addr(usb_device_handle_t dev_hdl);"""

# -- Host channel budget and endpoint-subset claims ----------------------------------------------
# Every pipe holds one of its controller's host channels from hcd_pipe_alloc() to hcd_pipe_free()
# (8 on the P4's full-speed controller, 16 on the high-speed one), the default pipe of every device
# included. A device that does not fit failed late: its driver's interface claim, or a stream the
# driver opens when it is used, ran out of channels ("No more HCD channels available").
#
# Two additions, both opt-in at the call site:
# - usb_host_interface_claim_endpoints(): a claim that leaves the endpoints in skip_endpoints
#   (bit = endpoint number, +16 for IN) without a pipe; usb_host_interface_claim() is that claim
#   with none skipped. A driver that never uses an endpoint (the interrupt OUT of a HID interface
#   whose output reports go over SET_REPORT) does not hold a channel for it.
# - usb_host_config_t::enum_budget_cb: asked for every device (hubs included) once its configuration
#   descriptor is read, before SET_CONFIGURATION, for the channels it takes once configured. Its
#   controller's commitments (usbh, per controller, given back when the device is freed) and that
#   need have to fit the controller's channels, or the enumeration is cancelled: the device is closed
#   (its default pipe and channel freed) and its port disabled, before any driver claims anything.
_BUDGET_PUBLIC_TYPE_ANCHOR = """typedef bool (*usb_host_enum_filter_cb_t)(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue);"""

_BUDGET_PUBLIC_TYPE_REPLACEMENT = """typedef bool (*usb_host_enum_filter_cb_t)(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue);

/**
 * @brief Host channels a device takes once configured
 *
 * Called for every device (hubs included) once its configuration descriptor is read, before
 * SET_CONFIGURATION: its default pipe plus every pipe its drivers will open. A device that does not
 * fit next to the devices already committed on its controller is not configured.
 *
 * @return Channels, or a negative value to take the device without counting it
 */
typedef int (*usb_host_enum_budget_cb_t)(const usb_device_desc_t *dev_desc, const usb_config_desc_t *config_desc,
                                         void *arg);"""

_BUDGET_PUBLIC_CONFIG_ANCHOR = """    usb_host_enum_filter_cb_t enum_filter_cb;   /**< Enumeration filter callback. Enable CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
                                                     to use this feature. Set to NULL otherwise. */"""

_BUDGET_PUBLIC_CONFIG_REPLACEMENT = """    usb_host_enum_filter_cb_t enum_filter_cb;   /**< Enumeration filter callback. Enable CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
                                                     to use this feature. Set to NULL otherwise. */
    usb_host_enum_budget_cb_t enum_budget_cb;   /**< Host channel budget callback, NULL: devices are not counted */
    void *enum_budget_cb_arg;                   /**< Argument of enum_budget_cb */"""

_CLAIM_PUBLIC_ANCHOR = """esp_err_t usb_host_interface_claim(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl, uint8_t bInterfaceNumber, uint8_t bAlternateSetting);"""

_CLAIM_PUBLIC_REPLACEMENT = """esp_err_t usb_host_interface_claim(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl, uint8_t bInterfaceNumber, uint8_t bAlternateSetting);

/**
 * @brief usb_host_interface_claim() that opens no pipe for the endpoints in skip_endpoints
 *
 * @param[in] skip_endpoints Endpoints of the alternate setting left without a pipe (and host channel): bit
 *                           (bEndpointAddress & 0x0F), plus 16 for an IN endpoint. 0 claims all of them.
 */
esp_err_t usb_host_interface_claim_endpoints(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl,
                                             uint8_t bInterfaceNumber, uint8_t bAlternateSetting,
                                             uint32_t skip_endpoints);

/**
 * @brief Host channels of a controller: allocated now, committed to configured devices, and its total
 *
 * @return ESP_ERR_INVALID_STATE until a device on that controller was counted
 */
esp_err_t usb_host_channel_usage(unsigned periph_idx, int *used, int *committed, int *total);"""

_CLAIM_PRIVATE_SIG_ANCHOR = """static esp_err_t interface_claim(client_t *client_obj, usb_device_handle_t dev_hdl, const usb_config_desc_t *config_desc, uint8_t bInterfaceNumber, uint8_t bAlternateSetting, interface_t **intf_obj_ret)"""

_CLAIM_PRIVATE_SIG_REPLACEMENT = """static esp_err_t interface_claim(client_t *client_obj, usb_device_handle_t dev_hdl, const usb_config_desc_t *config_desc, uint8_t bInterfaceNumber, uint8_t bAlternateSetting, uint32_t skip_endpoints, interface_t **intf_obj_ret)"""

_CLAIM_SKIP_ANCHOR = """        // Allocate the endpoint wrapper item
        ep_wrapper_t *ep_wrap;"""

_CLAIM_SKIP_REPLACEMENT = """        // An endpoint the driver never uses gets no pipe (and no host channel); its slot stays NULL
        const uint8_t ep_bit = (ep_desc->bEndpointAddress & 0x0F) + ((ep_desc->bEndpointAddress & 0x80) ? 16 : 0);
        if (skip_endpoints & (1u << ep_bit)) {
            continue;
        }
        // Allocate the endpoint wrapper item
        ep_wrapper_t *ep_wrap;"""

_CLAIM_INSERT_ANCHOR = """    for (int i = 0; i < intf_desc->bNumEndpoints; i++) {
        TAILQ_INSERT_TAIL(&client_obj->dynamic.idle_ep_tailq, intf_obj->constant.endpoints[i], dynamic.tailq_entry);
    }"""

_CLAIM_INSERT_REPLACEMENT = """    for (int i = 0; i < intf_desc->bNumEndpoints; i++) {
        if (intf_obj->constant.endpoints[i] != NULL) {
            TAILQ_INSERT_TAIL(&client_obj->dynamic.idle_ep_tailq, intf_obj->constant.endpoints[i], dynamic.tailq_entry);
        }
    }"""

_RELEASE_CHECK_ANCHOR = """        ep_wrapper_t *ep_wrap = intf_obj->constant.endpoints[i];
        // Endpoint must not be on the pending list and must not have in-flight URBs"""

_RELEASE_CHECK_REPLACEMENT = """        ep_wrapper_t *ep_wrap = intf_obj->constant.endpoints[i];
        if (ep_wrap == NULL) {
            continue;  // Skipped at the claim
        }
        // Endpoint must not be on the pending list and must not have in-flight URBs"""

_RELEASE_REMOVE_ANCHOR = """    for (int i = 0; i < intf_obj->constant.intf_desc->bNumEndpoints; i++) {
        TAILQ_REMOVE(&client_obj->dynamic.idle_ep_tailq, intf_obj->constant.endpoints[i], dynamic.tailq_entry);
    }"""

_RELEASE_REMOVE_REPLACEMENT = """    for (int i = 0; i < intf_obj->constant.intf_desc->bNumEndpoints; i++) {
        if (intf_obj->constant.endpoints[i] != NULL) {
            TAILQ_REMOVE(&client_obj->dynamic.idle_ep_tailq, intf_obj->constant.endpoints[i], dynamic.tailq_entry);
        }
    }"""

_CLAIM_PUBLIC_IMPL_ANCHOR = """esp_err_t usb_host_interface_claim(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl, uint8_t bInterfaceNumber, uint8_t bAlternateSetting)
{"""

_CLAIM_PUBLIC_IMPL_REPLACEMENT = """esp_err_t usb_host_interface_claim(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl, uint8_t bInterfaceNumber, uint8_t bAlternateSetting)
{
    return usb_host_interface_claim_endpoints(client_hdl, dev_hdl, bInterfaceNumber, bAlternateSetting, 0);
}

esp_err_t usb_host_channel_usage(unsigned periph_idx, int *used, int *committed, int *total)
{
    HOST_CHECK(used != NULL && committed != NULL && total != NULL, ESP_ERR_INVALID_ARG);
    return usbh_channel_usage(periph_idx, used, committed, total);
}

esp_err_t usb_host_interface_claim_endpoints(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl,
                                             uint8_t bInterfaceNumber, uint8_t bAlternateSetting,
                                             uint32_t skip_endpoints)
{"""

_CLAIM_PUBLIC_CALL_ANCHOR = """    ret = interface_claim(client_obj, dev_hdl, config_desc, bInterfaceNumber, bAlternateSetting, &intf_obj);"""

_CLAIM_PUBLIC_CALL_REPLACEMENT = """    ret = interface_claim(client_obj, dev_hdl, config_desc, bInterfaceNumber, bAlternateSetting, skip_endpoints, &intf_obj);"""

_BUDGET_INSTALL_ANCHOR = """        .enum_event_cb = enum_event_callback,
        .enum_event_cb_arg = NULL,"""

_BUDGET_INSTALL_REPLACEMENT = """        .enum_event_cb = enum_event_callback,
        .enum_event_cb_arg = NULL,
        .enum_budget_cb = config->enum_budget_cb,
        .enum_budget_cb_arg = config->enum_budget_cb_arg,"""

_BUDGET_ENUM_HEADER_ANCHOR = """    enum_event_cb_t enum_event_cb;                  /**< Enum event callback */
    void *enum_event_cb_arg;                        /**< Enum event callback argument */"""

_BUDGET_ENUM_HEADER_REPLACEMENT = """    enum_event_cb_t enum_event_cb;                  /**< Enum event callback */
    void *enum_event_cb_arg;                        /**< Enum event callback argument */
    usb_host_enum_budget_cb_t enum_budget_cb;       /**< Host channel budget callback, NULL: not counted */
    void *enum_budget_cb_arg;                       /**< Host channel budget callback argument */"""

_BUDGET_ENUM_FIELD_ANCHOR = """        enum_event_cb_t enum_event_cb;              /**< Enumeration driver event callback */
        void *enum_event_cb_arg;                    /**< Enumeration driver event callback argument */"""

_BUDGET_ENUM_FIELD_REPLACEMENT = """        enum_event_cb_t enum_event_cb;              /**< Enumeration driver event callback */
        void *enum_event_cb_arg;                    /**< Enumeration driver event callback argument */
        usb_host_enum_budget_cb_t enum_budget_cb;   /**< Host channel budget callback */
        void *enum_budget_cb_arg;                   /**< Host channel budget callback argument */"""

_BUDGET_ENUM_STORE_ANCHOR = """    enum_drv->constant.enum_event_cb_arg = config->enum_event_cb_arg;"""

_BUDGET_ENUM_STORE_REPLACEMENT = """    enum_drv->constant.enum_event_cb_arg = config->enum_event_cb_arg;
    enum_drv->constant.enum_budget_cb = config->enum_budget_cb;
    enum_drv->constant.enum_budget_cb_arg = config->enum_budget_cb_arg;"""

_BUDGET_ENUM_CHECK_ANCHOR = """    // Allocate Configuration descriptor and set it's value to device object
    ret = usbh_dev_set_config_desc(dev_hdl, config_desc);

exit:
    return ret;
}"""

_BUDGET_ENUM_CHECK_REPLACEMENT = """    // Allocate Configuration descriptor and set it's value to device object
    ret = usbh_dev_set_config_desc(dev_hdl, config_desc);
    // Its host channels have to fit on its controller, or it is not configured (the stage fails: cancelled)
    if (ret == ESP_OK && p_enum_driver->constant.enum_budget_cb != NULL) {
        const usb_device_desc_t *dev_desc = NULL;
        if (usbh_dev_get_desc(dev_hdl, &dev_desc) == ESP_OK && dev_desc != NULL) {
            const int channels = p_enum_driver->constant.enum_budget_cb(dev_desc, config_desc,
                                                                        p_enum_driver->constant.enum_budget_cb_arg);
            if (channels >= 0) {
                ret = usbh_dev_commit_channels(dev_hdl, channels);
            }
        }
    }

exit:
    return ret;
}"""

_BUDGET_USBH_FIELD_ANCHOR = """        usb_str_desc_t *str_desc_ser_num;           /**< Device's Serial string descriptor pointer */
    } constant;"""

_BUDGET_USBH_FIELD_REPLACEMENT = """        usb_str_desc_t *str_desc_ser_num;           /**< Device's Serial string descriptor pointer */
        uint8_t channels;                           /**< Host channels committed to the device (usbh_dev_commit_channels) */
    } constant;"""

_BUDGET_USBH_STATE_ANCHOR = """const char *USBH_TAG = "USBH";"""

_BUDGET_USBH_STATE_REPLACEMENT = """const char *USBH_TAG = "USBH";

// Host channels committed to the configured devices of each controller (usbh_dev_commit_channels()),
// and a port of that controller to read its channel count from.
static int s_channels_committed[SOC_USB_OTG_PERIPH_NUM];
static hcd_port_handle_t s_channel_port[SOC_USB_OTG_PERIPH_NUM];"""

_BUDGET_USBH_INCLUDE_ANCHOR = """#include "hcd.h"
#include "usbh.h\""""

_BUDGET_USBH_INCLUDE_REPLACEMENT = """#include "soc/soc_caps.h"
#include "hcd.h"
#include "usbh.h\""""

_BUDGET_USBH_FREE_ANCHOR = """    ESP_ERROR_CHECK(hcd_pipe_free(dev_obj->constant.default_pipe));
    heap_caps_free(dev_obj);
}"""

_BUDGET_USBH_FREE_REPLACEMENT = """    ESP_ERROR_CHECK(hcd_pipe_free(dev_obj->constant.default_pipe));
    if (dev_obj->constant.channels != 0) {
        const int periph = hcd_port_get_periph_idx(dev_obj->constant.port_hdl);
        USBH_ENTER_CRITICAL();
        s_channels_committed[periph] -= dev_obj->constant.channels;
        USBH_EXIT_CRITICAL();
    }
    heap_caps_free(dev_obj);
}"""

_BUDGET_USBH_COMMIT_ANCHOR = """esp_err_t usbh_dev_set_desc(usb_device_handle_t dev_hdl, const usb_device_desc_t *device_desc)
{"""

_BUDGET_USBH_COMMIT_REPLACEMENT = """esp_err_t usbh_dev_commit_channels(usb_device_handle_t dev_hdl, int channels)
{
    USBH_CHECK(dev_hdl != NULL && channels >= 0 && channels <= UINT8_MAX, ESP_ERR_INVALID_ARG);
    device_t *dev_obj = (device_t *)dev_hdl;
    hcd_port_handle_t port_hdl = dev_obj->constant.port_hdl;
    const int periph = hcd_port_get_periph_idx(port_hdl);
    int used = 0;
    int total = 0;
    hcd_port_get_channels(port_hdl, &used, &total);

    esp_err_t ret = ESP_OK;
    USBH_ENTER_CRITICAL();
    s_channel_port[periph] = port_hdl;
    const int committed = s_channels_committed[periph] - dev_obj->constant.channels;
    if (committed + channels > total) {
        ret = ESP_ERR_NOT_SUPPORTED;
    } else {
        s_channels_committed[periph] = committed + channels;
        dev_obj->constant.channels = (uint8_t)channels;
    }
    USBH_EXIT_CRITICAL();

    const usb_device_desc_t *desc = dev_obj->constant.desc;
    if (ret != ESP_OK) {
        ESP_LOGW(USBH_TAG, "Device %04X:%04X needs %d host channels, %d of %d are committed on controller %d: not configured",
                 desc ? desc->idVendor : 0, desc ? desc->idProduct : 0, channels, committed, total, periph);
    } else {
        ESP_LOGD(USBH_TAG, "Device %04X:%04X: %d host channels, controller %d %d of %d committed (%d allocated)",
                 desc ? desc->idVendor : 0, desc ? desc->idProduct : 0, channels, periph, committed + channels, total, used);
    }
    return ret;
}

esp_err_t usbh_channel_usage(unsigned periph_idx, int *used, int *committed, int *total)
{
    USBH_CHECK(periph_idx < SOC_USB_OTG_PERIPH_NUM, ESP_ERR_INVALID_ARG);
    USBH_ENTER_CRITICAL();
    hcd_port_handle_t port_hdl = s_channel_port[periph_idx];
    *committed = s_channels_committed[periph_idx];
    USBH_EXIT_CRITICAL();
    if (port_hdl == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    hcd_port_get_channels(port_hdl, used, total);
    return ESP_OK;
}

esp_err_t usbh_dev_set_desc(usb_device_handle_t dev_hdl, const usb_device_desc_t *device_desc)
{"""

_BUDGET_USBH_HEADER_ANCHOR = """esp_err_t usbh_dev_set_addr(usb_device_handle_t dev_hdl, uint8_t dev_addr);"""

_BUDGET_USBH_HEADER_REPLACEMENT = """esp_err_t usbh_dev_set_addr(usb_device_handle_t dev_hdl, uint8_t dev_addr);

/**
 * @brief Commit host channels to a device being enumerated
 *
 * They are given back when the device object is freed. ESP_ERR_NOT_SUPPORTED: they do not fit next to the
 * channels committed on the device's controller (logged), nothing committed.
 */
esp_err_t usbh_dev_commit_channels(usb_device_handle_t dev_hdl, int channels);

/**
 * @brief Host channels of a controller: allocated, committed (usbh_dev_commit_channels), total
 */
esp_err_t usbh_channel_usage(unsigned periph_idx, int *used, int *committed, int *total);"""

_BUDGET_HCD_HEADER_ANCHOR = """esp_err_t hcd_port_get_speed(hcd_port_handle_t port_hdl, usb_speed_t *speed);"""

_BUDGET_HCD_HEADER_REPLACEMENT = """esp_err_t hcd_port_get_speed(hcd_port_handle_t port_hdl, usb_speed_t *speed);

/**
 * @brief Peripheral (controller) index of a port
 */
int hcd_port_get_periph_idx(hcd_port_handle_t port_hdl);

/**
 * @brief Host channels of a port's controller: allocated now (one per pipe) and in total
 */
void hcd_port_get_channels(hcd_port_handle_t port_hdl, int *used, int *total);"""

_BUDGET_HCD_IMPL_ANCHOR = """hcd_port_event_t hcd_port_handle_event(hcd_port_handle_t port_hdl)
{"""

_BUDGET_HCD_IMPL_REPLACEMENT = """int hcd_port_get_periph_idx(hcd_port_handle_t port_hdl)
{
    return ((port_t *)port_hdl)->periph_idx;
}

void hcd_port_get_channels(hcd_port_handle_t port_hdl, int *used, int *total)
{
    port_t *port = (port_t *)port_hdl;
    HCD_ENTER_CRITICAL();
    *used = port->hal->channels.num_allocated;
    *total = (int)port->hal->constant_config.chan_num_total;
    HCD_EXIT_CRITICAL();
}

hcd_port_event_t hcd_port_handle_event(hcd_port_handle_t port_hdl)
{"""

_WAKE_INCLUDE_ANCHOR = """#include "freertos/FreeRTOS.h"
"""

_WAKE_INCLUDE_REPLACEMENT = """#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
"""

_WAKE_PUBLIC_ANCHOR = """esp_err_t usb_host_client_unblock(usb_host_client_handle_t client_hdl);
"""

_WAKE_PUBLIC_REPLACEMENT = """esp_err_t usb_host_client_unblock(usb_host_client_handle_t client_hdl);

void usb_host_client_set_wake_task(TaskHandle_t task);
"""

_WAKE_PRIVATE_ANCHOR = """static bool _unblock_client(client_t *client_obj, bool in_isr)
{
    bool yield;

    HOST_EXIT_CRITICAL_SAFE();
    if (in_isr) {
        BaseType_t xTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(client_obj->constant.event_sem, &xTaskWoken);
        yield = (xTaskWoken == pdTRUE);
    } else {
        xSemaphoreGive(client_obj->constant.event_sem);
        yield = false;
    }
    HOST_ENTER_CRITICAL_SAFE();

    return yield;
}
"""

_WAKE_PRIVATE_REPLACEMENT = """static TaskHandle_t s_client_wake_task = NULL;

void usb_host_client_set_wake_task(TaskHandle_t task)
{
    s_client_wake_task = task;
}

static bool _unblock_client(client_t *client_obj, bool in_isr)
{
    bool yield;

    HOST_EXIT_CRITICAL_SAFE();
    if (in_isr) {
        BaseType_t xTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(client_obj->constant.event_sem, &xTaskWoken);
        if (s_client_wake_task != NULL) {
            vTaskNotifyGiveFromISR(s_client_wake_task, &xTaskWoken);
        }
        yield = (xTaskWoken == pdTRUE);
    } else {
        xSemaphoreGive(client_obj->constant.event_sem);
        if (s_client_wake_task != NULL) {
            xTaskNotifyGive(s_client_wake_task);
        }
        yield = false;
    }
    HOST_ENTER_CRITICAL_SAFE();

    return yield;
}
"""


# (relative path, anchor, replacement) applied in order, each exactly once.
PATCHES = (
    ("include/usb/usb_host.h", _PUBLIC_INCLUDE_ANCHOR, _PUBLIC_INCLUDE_ADDITION),
    ("include/usb/usb_host.h", _PUBLIC_HEADER_ANCHOR, _PUBLIC_HEADER_ADDITION),
    ("include/usb/usb_host.h", _PUBLIC_CONFIG_ANCHOR, _PUBLIC_CONFIG_ADDITION),
    ("private_include/hcd.h", _HCD_HEADER_ANCHOR, _HCD_HEADER_REPLACEMENT),
    ("private_include/hub.h", _HUB_HEADER_ANCHOR, _HUB_HEADER_REPLACEMENT),
    ("src/hcd_dwc.c", _HCD_CALC_ANCHOR, _HCD_CALC_REPLACEMENT),
    ("src/hcd_dwc.c", _HCD_CALL_ANCHOR, _HCD_CALL_REPLACEMENT),
    ("src/hcd_dwc.c", _RESET_BAILOUT_ANCHOR, _RESET_BAILOUT_REPLACEMENT),
    ("src/hub.c", _HUB_PORT_ANCHOR, _HUB_PORT_REPLACEMENT),
    ("src/hub.c", _HUB_RESET_ERR_ANCHOR, _HUB_RESET_ERR_REPLACEMENT),
    ("src/usb_host.c", _HOST_WARN_ANCHOR, _HOST_WARN_REPLACEMENT),
    ("include/usb/usb_host.h", _FSLS_PUBLIC_ANCHOR, _FSLS_PUBLIC_REPLACEMENT),
    ("private_include/hcd.h", _FSLS_HCD_HEADER_ANCHOR, _FSLS_HCD_HEADER_REPLACEMENT),
    ("private_include/hub.h", _FSLS_HUB_HEADER_ANCHOR, _FSLS_HUB_HEADER_REPLACEMENT),
    ("src/hub.c", _FSLS_HUB_PORT_ANCHOR, _FSLS_HUB_PORT_REPLACEMENT),
    ("src/usb_host.c", _FSLS_HOST_ANCHOR, _FSLS_HOST_REPLACEMENT),
    ("src/hcd_dwc.c", _FSLS_PORT_FIELD_ANCHOR, _FSLS_PORT_FIELD_REPLACEMENT),
    ("src/hcd_dwc.c", _FSLS_PORT_INIT_ANCHOR, _FSLS_PORT_INIT_REPLACEMENT),
    ("src/hcd_dwc.c", _FSLS_RESET_ANCHOR, _FSLS_RESET_REPLACEMENT),
    ("src/ext_port.c", _PORT_FLAGS_ANCHOR, _PORT_FLAGS_REPLACEMENT),
    ("src/ext_port.c", _PORT_STATUS_REQ_ANCHOR, _PORT_STATUS_REQ_REPLACEMENT),
    ("src/ext_port.c", _PORT_SET_FEATURE_ANCHOR, _PORT_SET_FEATURE_REPLACEMENT),
    ("src/ext_port.c", _PORT_CLEAR_FEATURE_ANCHOR, _PORT_CLEAR_FEATURE_REPLACEMENT),
    ("src/ext_port.c", _PORT_COMPLETE_ANCHOR, _PORT_COMPLETE_REPLACEMENT),
    ("src/ext_port.c", _PORT_RESETTING_ANCHOR, _PORT_RESETTING_REPLACEMENT),
    ("src/ext_port.c", _PORT_ENABLED_ANCHOR, _PORT_ENABLED_REPLACEMENT),
    ("src/ext_port.c", _PORT_RECYCLE_ANCHOR, _PORT_RECYCLE_REPLACEMENT),
    ("src/ext_port.c", _PORT_ACTIVE_ANCHOR, _PORT_ACTIVE_REPLACEMENT),
    ("src/ext_port.c", _PORT_GET_STATUS_ANCHOR, _PORT_GET_STATUS_REPLACEMENT),
    ("src/ext_port.c", _PORT_SET_STATUS_ANCHOR, _PORT_SET_STATUS_REPLACEMENT),
    ("src/ext_port.c", _PORT_REQ_PROCESS_ANCHOR, _PORT_REQ_PROCESS_REPLACEMENT),
    ("src/ext_port.c", _PORT_PROCESS_STATUS_ANCHOR, _PORT_PROCESS_STATUS_REPLACEMENT),
    ("src/ext_hub.c", _HUB_PORT_FEATURE_ANCHOR, _HUB_PORT_FEATURE_REPLACEMENT),
    ("src/ext_hub.c", _HUB_PORT_STATUS_ANCHOR, _HUB_PORT_STATUS_REPLACEMENT),
    ("src/ext_hub.c", _HUB_REQUEST_ANCHOR, _HUB_REQUEST_REPLACEMENT),
    ("src/ext_hub.c", _HUB_STATUS_CHANGE_ANCHOR, _HUB_STATUS_CHANGE_REPLACEMENT),
    ("src/hcd_dwc.c", _HALT_ON_ERROR_ANCHOR, _HALT_ON_ERROR_REPLACEMENT),
    ("src/hcd_dwc.c", _HALT_DONE_ANCHOR, _HALT_DONE_REPLACEMENT),
    ("src/ext_port.c", _PORT_POWER_FIRST_ANCHOR, _PORT_POWER_FIRST_REPLACEMENT),
    ("src/ext_port.c", _PORT_POWER_FIRST_LOOP_ANCHOR, _PORT_POWER_FIRST_LOOP_REPLACEMENT),
    ("src/enum.c", _ENUM_PROBE_FIELD_ANCHOR, _ENUM_PROBE_FIELD_REPLACEMENT),
    ("src/enum.c", _ENUM_PROBE_STATUS_ANCHOR, _ENUM_PROBE_STATUS_REPLACEMENT),
    ("src/enum.c", _ENUM_PROBE_PARSE_ANCHOR, _ENUM_PROBE_PARSE_REPLACEMENT),
    ("src/enum.c", _ENUM_PROBE_NEXT_ANCHOR, _ENUM_PROBE_NEXT_REPLACEMENT),
    ("src/enum.c", _ENUM_PROBE_FAIL_ANCHOR, _ENUM_PROBE_FAIL_REPLACEMENT),
    ("src/usbh.c", _USBH_CLEAR_ADDR_ANCHOR, _USBH_CLEAR_ADDR_REPLACEMENT),
    ("private_include/usbh.h", _USBH_CLEAR_ADDR_HEADER_ANCHOR, _USBH_CLEAR_ADDR_HEADER_REPLACEMENT),
    ("include/usb/usb_types_stack.h", _BUDGET_PUBLIC_TYPE_ANCHOR, _BUDGET_PUBLIC_TYPE_REPLACEMENT),
    ("include/usb/usb_host.h", _BUDGET_PUBLIC_CONFIG_ANCHOR, _BUDGET_PUBLIC_CONFIG_REPLACEMENT),
    ("include/usb/usb_host.h", _CLAIM_PUBLIC_ANCHOR, _CLAIM_PUBLIC_REPLACEMENT),
    ("src/usb_host.c", _CLAIM_PRIVATE_SIG_ANCHOR, _CLAIM_PRIVATE_SIG_REPLACEMENT),
    ("src/usb_host.c", _CLAIM_SKIP_ANCHOR, _CLAIM_SKIP_REPLACEMENT),
    ("src/usb_host.c", _CLAIM_INSERT_ANCHOR, _CLAIM_INSERT_REPLACEMENT),
    ("src/usb_host.c", _RELEASE_CHECK_ANCHOR, _RELEASE_CHECK_REPLACEMENT),
    ("src/usb_host.c", _RELEASE_REMOVE_ANCHOR, _RELEASE_REMOVE_REPLACEMENT),
    ("src/usb_host.c", _CLAIM_PUBLIC_IMPL_ANCHOR, _CLAIM_PUBLIC_IMPL_REPLACEMENT),
    ("src/usb_host.c", _CLAIM_PUBLIC_CALL_ANCHOR, _CLAIM_PUBLIC_CALL_REPLACEMENT),
    ("src/usb_host.c", _BUDGET_INSTALL_ANCHOR, _BUDGET_INSTALL_REPLACEMENT),
    ("private_include/enum.h", _BUDGET_ENUM_HEADER_ANCHOR, _BUDGET_ENUM_HEADER_REPLACEMENT),
    ("src/enum.c", _BUDGET_ENUM_FIELD_ANCHOR, _BUDGET_ENUM_FIELD_REPLACEMENT),
    ("src/enum.c", _BUDGET_ENUM_STORE_ANCHOR, _BUDGET_ENUM_STORE_REPLACEMENT),
    ("src/enum.c", _BUDGET_ENUM_CHECK_ANCHOR, _BUDGET_ENUM_CHECK_REPLACEMENT),
    ("src/usbh.c", _BUDGET_USBH_INCLUDE_ANCHOR, _BUDGET_USBH_INCLUDE_REPLACEMENT),
    ("src/usbh.c", _BUDGET_USBH_FIELD_ANCHOR, _BUDGET_USBH_FIELD_REPLACEMENT),
    ("src/usbh.c", _BUDGET_USBH_STATE_ANCHOR, _BUDGET_USBH_STATE_REPLACEMENT),
    ("src/usbh.c", _BUDGET_USBH_FREE_ANCHOR, _BUDGET_USBH_FREE_REPLACEMENT),
    ("src/usbh.c", _BUDGET_USBH_COMMIT_ANCHOR, _BUDGET_USBH_COMMIT_REPLACEMENT),
    ("private_include/usbh.h", _BUDGET_USBH_HEADER_ANCHOR, _BUDGET_USBH_HEADER_REPLACEMENT),
    ("private_include/hcd.h", _BUDGET_HCD_HEADER_ANCHOR, _BUDGET_HCD_HEADER_REPLACEMENT),
    ("src/hcd_dwc.c", _BUDGET_HCD_IMPL_ANCHOR, _BUDGET_HCD_IMPL_REPLACEMENT),
    ("include/usb/usb_host.h", _WAKE_INCLUDE_ANCHOR, _WAKE_INCLUDE_REPLACEMENT),
    ("include/usb/usb_host.h", _WAKE_PUBLIC_ANCHOR, _WAKE_PUBLIC_REPLACEMENT),
    ("src/usb_host.c", _WAKE_PRIVATE_ANCHOR, _WAKE_PRIVATE_REPLACEMENT),
)

# Identifies an existing patched copy. It has to change whenever anything about that copy
# would change, or a copy left over from an earlier revision is trusted and silently used:
# hence a digest over the patch bodies themselves rather than a hand-maintained number.
PATCH_STAMP = "v1:{}:{}".format(
    USB_COMPONENT_VERSION,
    hashlib.sha256(
        "\0".join(part for patch in PATCHES for part in patch).encode()
    ).hexdigest()[:16],
)
