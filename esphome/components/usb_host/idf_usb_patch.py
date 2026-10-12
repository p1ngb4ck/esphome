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

_LS_PREAMBLE_ANCHOR = """static inline bool _buffer_check_done(pipe_t *pipe)
{
    // Only control transfers need to be continued
    if (pipe->ep_char.type != USB_DWC_XFER_TYPE_CTRL) {
        return true;
    }
    // The HW can't handle two transactions with preamble in one frame.
    // TODO: IDF-12986
    if (pipe->ep_char.ls_via_fs_hub) {
        esp_rom_delay_us(1000);
    }"""

_LS_PREAMBLE_REPLACEMENT = """/**
 * @brief Wait for the frame counter to advance before the next PREamble transaction
 *
 * The controller cannot issue two PRE-qualified transactions in the same frame. Sleeping
 * for one frame's worth of microseconds only works when the stage completed near the start
 * of a frame: a completion just before the counter rolls over is followed by another
 * transaction in the very next frame, which is the case this is meant to prevent. Watch the
 * counter itself instead.
 *
 * The bound is what keeps this safe to call from an interrupt: a stopped frame clock, which
 * is what a disconnect during a control transfer looks like, must not wedge the handler. A
 * full-speed frame is 1 ms, so 3 ms is well past any legitimate transition.
 */
static inline void _wait_next_preamble_frame(pipe_t *pipe)
{
    const uint32_t start_frame = usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal);
    for (int waited_us = 0; waited_us < 3000; waited_us += 10) {
        if (usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal) != start_frame) {
            return;
        }
        esp_rom_delay_us(10);
    }
}

static inline bool _buffer_check_done(pipe_t *pipe)
{
    // Only control transfers need to be continued
    if (pipe->ep_char.type != USB_DWC_XFER_TYPE_CTRL) {
        return true;
    }
    // The HW can't handle two transactions with preamble in one frame.
    if (pipe->ep_char.ls_via_fs_hub) {
        _wait_next_preamble_frame(pipe);
    }"""

_LS_EP0_ANCHOR = """    ep_char->ls_via_fs_hub = 0;
    if (pipe_idx > 0) {
        // TODO: remove warning after IDF-12986
        if (port_speed == USB_SPEED_FULL && pipe_config->dev_speed == USB_SPEED_LOW) {
            ESP_LOGW(HCD_DWC_TAG, "Low-speed, extra delay will be applied in ISR");
            ep_char->ls_via_fs_hub = 1;
        }
    }"""

_LS_EP0_REPLACEMENT = """    // A low-speed device reached through a full-speed hub needs a PREamble before every
    // transaction, the default control pipe included. Excluding pipe index 0 left that pipe
    // without it, so the first GET_DESCRIPTOR of enumeration went out unqualified and the
    // device never answered -- no endpoint of such a device could ever be reached, which is
    // why the exclusion was not noticed. A device on the root port is unaffected either way:
    // there the port speed is LOW, not FULL.
    ep_char->ls_via_fs_hub = 0;
    if (port_speed == USB_SPEED_FULL && pipe_config->dev_speed == USB_SPEED_LOW) {
        ESP_LOGD(HCD_DWC_TAG, "Low-speed device behind a full-speed hub, PREamble enabled");
        ep_char->ls_via_fs_hub = 1;
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

# A low-speed device behind a full-speed hub: the controller can lose a PRE-qualified
# transaction with a transaction error, most often the first one a new pipe launches, right
# after the hub's port reset. From NonaSuomy's ESP32-P4 USB host work: the first launch of a
# pipe waits for a frame boundary, and a control stage that fails that way is run again on a
# later frame with a rebuilt descriptor (the controller writes the descriptor back in place),
# a bounded number of times before the error reaches the transfer.
_LS_PIPE_FIELDS_ANCHOR = """    usb_dwc_hal_ep_char_t ep_char;
    // Port related"""

_LS_PIPE_FIELDS_REPLACEMENT = """    usb_dwc_hal_ep_char_t ep_char;
    uint8_t ls_ctrl_retry_count;            // Retries of the current control stage (LS via FS hub)
    bool ls_first_launch_pending;           // First PRE-qualified launch waits for a frame boundary
    // Port related"""

_LS_PIPE_ALLOC_ANCHOR = """    memcpy(&pipe->ep_char, &ep_char, sizeof(usb_dwc_hal_ep_char_t));
    pipe->state = HCD_PIPE_STATE_ACTIVE;"""

_LS_PIPE_ALLOC_REPLACEMENT = """    memcpy(&pipe->ep_char, &ep_char, sizeof(usb_dwc_hal_ep_char_t));
    pipe->ls_first_launch_pending = pipe->ep_char.ls_via_fs_hub;
    pipe->state = HCD_PIPE_STATE_ACTIVE;"""

_LS_EXEC_ANCHOR = """    dma_buffer_block_t *buffer_to_exec = pipe->buffers[pipe->multi_buffer_control.rd_idx];
    assert(buffer_to_exec->urb != NULL);

    uint32_t start_idx;"""

_LS_EXEC_REPLACEMENT = """    dma_buffer_block_t *buffer_to_exec = pipe->buffers[pipe->multi_buffer_control.rd_idx];
    assert(buffer_to_exec->urb != NULL);
    pipe->ls_ctrl_retry_count = 0;
    if (pipe->ls_first_launch_pending) {
        pipe->ls_first_launch_pending = false;
        _wait_next_preamble_frame(pipe);
    }

    uint32_t start_idx;"""

_LS_EXEC_CONT_ANCHOR = """    // Continue the control transfer
    usb_dwc_hal_chan_set_dir(pipe->chan_obj, next_dir_is_in);"""

_LS_EXEC_CONT_REPLACEMENT = """    // Continue the control transfer
    pipe->ls_ctrl_retry_count = 0;
    usb_dwc_hal_chan_set_dir(pipe->chan_obj, next_dir_is_in);"""

_LS_RETRY_HELPER_ANCHOR = """/**
 * @brief Handles a HAL channel interrupt
"""

_LS_RETRY_HELPER_REPLACEMENT = """#define LS_CTRL_RETRY_MAX 3

/**
 * @brief Run the current stage of a control transfer again, on a later frame
 *
 * Descriptor DMA writes the stage's descriptor back in place before it reports the error,
 * so it is rebuilt from the URB, as _buffer_fill_ctrl() built it.
 */
static inline void _ls_ctrl_retry(pipe_t *pipe)
{
    dma_buffer_block_t *buffer = pipe->buffers[pipe->multi_buffer_control.rd_idx];
    usb_transfer_t *transfer = &buffer->urb->transfer;
    const int stage = buffer->flags.ctrl.cur_stg;
    bool is_in;
    int pid;
    uint8_t *data;
    int len;
    uint32_t flags = USB_DWC_HAL_XFER_DESC_FLAG_HOC;
    if (stage == 0) {
        is_in = false;
        pid = 0;
        data = transfer->data_buffer;
        len = sizeof(usb_setup_packet_t);
        flags |= USB_DWC_HAL_XFER_DESC_FLAG_SETUP;
    } else if (stage == 1) {
        is_in = buffer->flags.ctrl.data_stg_in;
        pid = 1;
        data = transfer->data_buffer + sizeof(usb_setup_packet_t);
        len = transfer->num_bytes - sizeof(usb_setup_packet_t);
    } else {
        is_in = !(buffer->flags.ctrl.data_stg_in && !buffer->flags.ctrl.data_stg_skip);
        pid = 1;
        data = NULL;
        len = 0;
    }
    if (is_in) {
        flags |= USB_DWC_HAL_XFER_DESC_FLAG_IN;
    }
    usb_dwc_hal_xfer_desc_fill(buffer->xfer_desc_list, stage, data, len, flags);
    CACHE_SYNC_XFER_DESCRIPTOR_LIST_C2M(buffer);
    usb_dwc_hal_chan_set_dir(pipe->chan_obj, is_in);
    usb_dwc_hal_chan_set_pid(pipe->chan_obj, pid);
    _wait_next_preamble_frame(pipe);
    usb_dwc_hal_chan_activate(pipe->chan_obj, buffer->xfer_desc_list, XFER_LIST_LEN_CTRL, stage);
}

/**
 * @brief Handles a HAL channel interrupt
"""

_LS_RETRY_ANCHOR = """        // Get and store the pipe error event
        usb_dwc_hal_chan_error_t chan_error = usb_dwc_hal_chan_get_error(chan_obj);
        pipe->last_event = pipe_decode_error_event(chan_error);"""

_LS_RETRY_REPLACEMENT = """        // Get and store the pipe error event
        usb_dwc_hal_chan_error_t chan_error = usb_dwc_hal_chan_get_error(chan_obj);
        if (pipe->ep_char.ls_via_fs_hub && pipe->ep_char.type == USB_DWC_XFER_TYPE_CTRL &&
                chan_error == USB_DWC_HAL_CHAN_ERROR_XCS_XACT && pipe->ls_ctrl_retry_count < LS_CTRL_RETRY_MAX &&
                pipe->port->flags.conn_dev_ena && !pipe->cs_flags.waiting_halt) {
            pipe->ls_ctrl_retry_count++;
            _ls_ctrl_retry(pipe);
            break;
        }
        pipe->last_event = pipe_decode_error_event(chan_error);"""

# A low-speed device behind a full-speed hub, part two: the controller cannot issue two
# PRE-qualified transactions in one frame (IDF-12986), and the data stage of a control
# transfer is one descriptor covering every packet of it, so anything longer than one packet
# (an 18 byte device descriptor on an 8 byte EP0) failed from its second packet on. For such
# pipes the data stage now runs one packet per descriptor, a frame apart, continued with the
# next DATA toggle until a short packet or the end; the transfer reports the bytes moved.
_LS_FLAGS_ANCHOR = """            uint32_t cur_stg: 2;            // Index of the current stage (e.g., 0 is setup stage, 2 is status stage)
            uint32_t reserved28: 28;"""

_LS_FLAGS_REPLACEMENT = """            uint32_t cur_stg: 2;            // Index of the current stage (e.g., 0 is setup stage, 2 is status stage)
            uint32_t ls_split: 1;           // Data stage runs one packet at a time (LS via FS hub)
            uint32_t ls_data_done: 16;      // Data stage bytes moved so far when ls_split
            uint32_t reserved11: 11;"""

_LS_PACKET_HELPERS_ANCHOR = """#define LS_CTRL_RETRY_MAX 3
"""

_LS_PACKET_HELPERS_REPLACEMENT = """#define LS_CTRL_RETRY_MAX 3

static inline int _ls_ctrl_data_pid(const dma_buffer_block_t *buffer, int mps)
{
    // DATA1 first, then alternating
    return ((buffer->flags.ctrl.ls_data_done / mps) & 1) ? 0 : 1;
}

static inline int _ls_ctrl_packet_len(const dma_buffer_block_t *buffer, const usb_transfer_t *transfer, int mps)
{
    const int remaining = transfer->num_bytes - (int)sizeof(usb_setup_packet_t) - (int)buffer->flags.ctrl.ls_data_done;
    return remaining < mps ? remaining : mps;
}

static inline void _ls_ctrl_fill_data_packet(dma_buffer_block_t *buffer, usb_transfer_t *transfer, int mps)
{
    usb_dwc_hal_xfer_desc_fill(buffer->xfer_desc_list, 1,
                               transfer->data_buffer + sizeof(usb_setup_packet_t) + buffer->flags.ctrl.ls_data_done,
                               _ls_ctrl_packet_len(buffer, transfer, mps),
                               (buffer->flags.ctrl.data_stg_in ? USB_DWC_HAL_XFER_DESC_FLAG_IN : 0) | USB_DWC_HAL_XFER_DESC_FLAG_HOC);
}
"""

_LS_RETRY_PACKET_ANCHOR = """    } else if (stage == 1) {
        is_in = buffer->flags.ctrl.data_stg_in;
        pid = 1;
        data = transfer->data_buffer + sizeof(usb_setup_packet_t);
        len = transfer->num_bytes - sizeof(usb_setup_packet_t);
    } else {"""

_LS_RETRY_PACKET_REPLACEMENT = """    } else if (stage == 1) {
        is_in = buffer->flags.ctrl.data_stg_in;
        if (buffer->flags.ctrl.ls_split) {
            pid = _ls_ctrl_data_pid(buffer, pipe->ep_char.mps);
            data = transfer->data_buffer + sizeof(usb_setup_packet_t) + buffer->flags.ctrl.ls_data_done;
            len = _ls_ctrl_packet_len(buffer, transfer, pipe->ep_char.mps);
        } else {
            pid = 1;
            data = transfer->data_buffer + sizeof(usb_setup_packet_t);
            len = transfer->num_bytes - sizeof(usb_setup_packet_t);
        }
    } else {"""

_LS_FILL_ANCHOR = """        _buffer_fill_ctrl(buffer_to_fill, transfer);
        break;"""

_LS_FILL_REPLACEMENT = """        _buffer_fill_ctrl(buffer_to_fill, transfer);
        buffer_to_fill->flags.ctrl.ls_split = 0;
        if (pipe->ep_char.ls_via_fs_hub && !buffer_to_fill->flags.ctrl.data_stg_skip) {
            buffer_to_fill->flags.ctrl.ls_split = 1;
            buffer_to_fill->flags.ctrl.ls_data_done = 0;
            _ls_ctrl_fill_data_packet(buffer_to_fill, transfer, mps);
        }
        break;"""

_LS_CONT_ANCHOR = """    if (buffer_inflight->flags.ctrl.cur_stg == 0) { // Just finished control stage"""

_LS_CONT_REPLACEMENT = """    if (buffer_inflight->flags.ctrl.cur_stg == 1 && buffer_inflight->flags.ctrl.ls_split) {
        usb_transfer_t *transfer = &buffer_inflight->urb->transfer;
        const int mps = pipe->ep_char.mps;
        const int programmed = _ls_ctrl_packet_len(buffer_inflight, transfer, mps);
        int rem_len;
        int desc_status;
        CACHE_SYNC_XFER_DESCRIPTOR_LIST_M2C(buffer_inflight);
        usb_dwc_hal_xfer_desc_parse(buffer_inflight->xfer_desc_list, 1, &rem_len, &desc_status);
        const int moved = programmed - rem_len;
        buffer_inflight->flags.ctrl.ls_data_done += moved;
        if (moved == programmed && _ls_ctrl_packet_len(buffer_inflight, transfer, mps) > 0) {
            // Next packet of the data stage; _buffer_check_done() already waited for a new frame
            pipe->ls_ctrl_retry_count = 0;
            _ls_ctrl_fill_data_packet(buffer_inflight, transfer, mps);
            CACHE_SYNC_XFER_DESCRIPTOR_LIST_C2M(buffer_inflight);
            usb_dwc_hal_chan_set_dir(pipe->chan_obj, buffer_inflight->flags.ctrl.data_stg_in);
            usb_dwc_hal_chan_set_pid(pipe->chan_obj, _ls_ctrl_data_pid(buffer_inflight, mps));
            usb_dwc_hal_chan_activate(pipe->chan_obj, buffer_inflight->xfer_desc_list, XFER_LIST_LEN_CTRL, 1);
            return;
        }
    }
    if (buffer_inflight->flags.ctrl.cur_stg == 0) { // Just finished control stage"""

_LS_PARSE_ANCHOR = """    if (buffer->flags.ctrl.data_stg_skip)     {
        // There was no data stage. Just set the actual length to the size of the setup packet
        transfer->actual_num_bytes = sizeof(usb_setup_packet_t);
    } else {"""

_LS_PARSE_REPLACEMENT = """    if (buffer->flags.ctrl.data_stg_skip)     {
        // There was no data stage. Just set the actual length to the size of the setup packet
        transfer->actual_num_bytes = sizeof(usb_setup_packet_t);
    } else if (buffer->flags.ctrl.ls_split) {
        transfer->actual_num_bytes = sizeof(usb_setup_packet_t) + buffer->flags.ctrl.ls_data_done;
    } else {"""

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
#if CONFIG_IDF_TARGET_ESP32P4
    if (port->fsls_only) {
        usb_dwc_ll_hcfg_set_fsls_supp_only(port->hal->dev);
    }
#endif
    usb_dwc_hal_port_toggle_reset(port->hal, true);"""

# (relative path, anchor, replacement) applied in order, each exactly once.
PATCHES = (
    ("include/usb/usb_host.h", _PUBLIC_INCLUDE_ANCHOR, _PUBLIC_INCLUDE_ADDITION),
    ("include/usb/usb_host.h", _PUBLIC_HEADER_ANCHOR, _PUBLIC_HEADER_ADDITION),
    ("include/usb/usb_host.h", _PUBLIC_CONFIG_ANCHOR, _PUBLIC_CONFIG_ADDITION),
    ("private_include/hcd.h", _HCD_HEADER_ANCHOR, _HCD_HEADER_REPLACEMENT),
    ("private_include/hub.h", _HUB_HEADER_ANCHOR, _HUB_HEADER_REPLACEMENT),
    ("src/hcd_dwc.c", _HCD_CALC_ANCHOR, _HCD_CALC_REPLACEMENT),
    ("src/hcd_dwc.c", _HCD_CALL_ANCHOR, _HCD_CALL_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PREAMBLE_ANCHOR, _LS_PREAMBLE_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_EP0_ANCHOR, _LS_EP0_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PIPE_FIELDS_ANCHOR, _LS_PIPE_FIELDS_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PIPE_ALLOC_ANCHOR, _LS_PIPE_ALLOC_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_EXEC_ANCHOR, _LS_EXEC_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_EXEC_CONT_ANCHOR, _LS_EXEC_CONT_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_RETRY_HELPER_ANCHOR, _LS_RETRY_HELPER_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_RETRY_ANCHOR, _LS_RETRY_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_FLAGS_ANCHOR, _LS_FLAGS_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PACKET_HELPERS_ANCHOR, _LS_PACKET_HELPERS_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_RETRY_PACKET_ANCHOR, _LS_RETRY_PACKET_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_FILL_ANCHOR, _LS_FILL_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_CONT_ANCHOR, _LS_CONT_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PARSE_ANCHOR, _LS_PARSE_REPLACEMENT),
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
