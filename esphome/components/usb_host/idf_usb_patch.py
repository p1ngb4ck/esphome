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
    uint8_t ls_intr_retry_count;            // Interrupt transfer retries in the current run (LS via FS hub)
    uint16_t ls_intr_retry_frame;           // Frame of the last interrupt transfer retry
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

/*
 * The data stage of a control transfer to a low-speed device behind a full-speed hub, one packet
 * per frame. The controller cannot issue two PRE-qualified transactions in one frame (see
 * _buffer_check_done()), and a data stage of several packets in one channel activation is
 * exactly that: the device descriptor is 3 packets, a HID report descriptor often more than 10.
 * Each piece is one packet, launched after the frame counter moved on; bytes done are kept in
 * the buffer's control flags (ls_done), which _buffer_parse_ctrl() then reports.
 */
static inline int _ls_ctrl_data_len(dma_buffer_block_t *buffer)
{
    usb_transfer_t *transfer = &buffer->urb->transfer;
    const usb_setup_packet_t *setup = (const usb_setup_packet_t *)transfer->data_buffer;
    const int len = transfer->num_bytes - (int)sizeof(usb_setup_packet_t);
    return (setup->wLength < len) ? setup->wLength : len;
}

static inline int _ls_ctrl_data_piece(pipe_t *pipe, dma_buffer_block_t *buffer)
{
    const int mps = pipe->ep_char.mps;
    const int rem = _ls_ctrl_data_len(buffer) - (int)buffer->flags.ctrl.ls_done;
    // An IN piece asks for a whole packet: the buffer holds whole packets, a short one ends the stage
    return (buffer->flags.ctrl.data_stg_in || rem > mps) ? mps : rem;
}

static inline int _ls_ctrl_data_pid(pipe_t *pipe, dma_buffer_block_t *buffer)
{
    // DATA1 first, then alternating
    return ((buffer->flags.ctrl.ls_done / pipe->ep_char.mps) & 1) ? 0 : 1;
}

static inline void _ls_ctrl_data_fill(pipe_t *pipe, dma_buffer_block_t *buffer)
{
    usb_transfer_t *transfer = &buffer->urb->transfer;
    usb_dwc_hal_xfer_desc_fill(buffer->xfer_desc_list, 1,
                               transfer->data_buffer + sizeof(usb_setup_packet_t) + buffer->flags.ctrl.ls_done,
                               _ls_ctrl_data_piece(pipe, buffer),
                               (buffer->flags.ctrl.data_stg_in ? USB_DWC_HAL_XFER_DESC_FLAG_IN : 0) |
                               USB_DWC_HAL_XFER_DESC_FLAG_HOC);
    CACHE_SYNC_XFER_DESCRIPTOR_LIST_C2M(buffer);
}

static inline void _ls_ctrl_data_start(pipe_t *pipe, dma_buffer_block_t *buffer)
{
    buffer->flags.ctrl.ls_split = 1;
    buffer->flags.ctrl.ls_done = 0;
    _ls_ctrl_data_fill(pipe, buffer);
}

/**
 * @brief A data stage piece completed: launch the next one
 *
 * @return true when another piece was launched, false when the data stage is done (a short
 *         packet, or all bytes of it) and the status stage follows
 */
static inline bool _ls_ctrl_data_next(pipe_t *pipe, dma_buffer_block_t *buffer)
{
    const int asked = _ls_ctrl_data_piece(pipe, buffer);
    int rem_len;
    int desc_status;
    CACHE_SYNC_XFER_DESCRIPTOR_LIST_M2C(buffer);
    usb_dwc_hal_xfer_desc_parse(buffer->xfer_desc_list, 1, &rem_len, &desc_status);
    const int got = asked - rem_len;
    buffer->flags.ctrl.ls_done += got;
    if (got < pipe->ep_char.mps || (int)buffer->flags.ctrl.ls_done >= _ls_ctrl_data_len(buffer)) {
        return false;
    }
    pipe->ls_ctrl_retry_count = 0;
    _ls_ctrl_data_fill(pipe, buffer);
    usb_dwc_hal_chan_set_dir(pipe->chan_obj, buffer->flags.ctrl.data_stg_in);
    usb_dwc_hal_chan_set_pid(pipe->chan_obj, _ls_ctrl_data_pid(pipe, buffer));
    usb_dwc_hal_chan_activate(pipe->chan_obj, buffer->xfer_desc_list, XFER_LIST_LEN_CTRL, 1);
    return true;
}

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
        pid = _ls_ctrl_data_pid(pipe, buffer);
        data = transfer->data_buffer + sizeof(usb_setup_packet_t) + buffer->flags.ctrl.ls_done;
        len = _ls_ctrl_data_piece(pipe, buffer);
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

#define LS_INTR_RETRY_WINDOW 1024   // Frames: errors further apart start a new run of retries

/**
 * @brief Run an interrupt transfer again from its first packet not done, on a later frame
 *
 * A lost PRE-qualified transaction halts the channel like a dead device does. Descriptors done
 * keep their status, the rest are rebuilt as _buffer_fill_intr() built them; the data toggle
 * stays in HCTSIZ across the activation. Bounded per run, so a device that is gone still ends
 * the transfer with the error.
 *
 * @return true when the transfer runs again
 */
static inline bool _ls_intr_retry(pipe_t *pipe)
{
    const uint16_t frame = usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal);
    if (((uint16_t)(frame - pipe->ls_intr_retry_frame) & 0x3FFF) >= LS_INTR_RETRY_WINDOW) {
        pipe->ls_intr_retry_count = 0;
    }
    if (pipe->ls_intr_retry_count >= LS_CTRL_RETRY_MAX) {
        return false;
    }
    dma_buffer_block_t *buffer = pipe->buffers[pipe->multi_buffer_control.rd_idx];
    usb_transfer_t *transfer = &buffer->urb->transfer;
    const int mps = pipe->ep_char.mps;
    const int num_qtds = buffer->flags.intr.num_qtds;
    const bool zero_len_packet = buffer->flags.intr.zero_len_packet;
    const int list_len = num_qtds + (zero_len_packet ? 1 : 0);
    const uint32_t dir_flag = (pipe->ep_char.bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) ? USB_DWC_HAL_XFER_DESC_FLAG_IN : 0;
    CACHE_SYNC_XFER_DESCRIPTOR_LIST_M2C(buffer);
    int first = 0;
    for (; first < list_len; first++) {
        int rem_len;
        int desc_status;
        usb_dwc_hal_xfer_desc_parse(buffer->xfer_desc_list, first, &rem_len, &desc_status);
        if (rem_len != 0 || desc_status != USB_DWC_HAL_XFER_DESC_STS_SUCCESS) {
            break;
        }
    }
    if (first >= list_len) {
        return false;
    }
    pipe->ls_intr_retry_count++;
    pipe->ls_intr_retry_frame = frame;
    for (int i = first; i < num_qtds; i++) {
        const bool last = (i == num_qtds - 1);
        usb_dwc_hal_xfer_desc_fill(buffer->xfer_desc_list, i, &transfer->data_buffer[i * mps],
                                   last ? transfer->num_bytes - i * mps : mps,
                                   dir_flag | ((last && !zero_len_packet) ? USB_DWC_HAL_XFER_DESC_FLAG_HOC : 0));
    }
    if (zero_len_packet) {
        usb_dwc_hal_xfer_desc_fill(buffer->xfer_desc_list, num_qtds, NULL, 0, USB_DWC_HAL_XFER_DESC_FLAG_HOC);
    }
    CACHE_SYNC_XFER_DESCRIPTOR_LIST_C2M(buffer);
    _wait_next_preamble_frame(pipe);
    usb_dwc_hal_chan_activate(pipe->chan_obj, buffer->xfer_desc_list, list_len, first);
    return true;
}

/**
 * @brief Give a low-speed periodic pipe behind a full-speed hub frames of its own
 *
 * Two PRE-qualified transactions must not share a frame. The offset moves to the first phase
 * whose frames no such pipe polls in yet; with none left the pipe keeps its own.
 */
static inline void _ls_periodic_set_offset(port_t *port, usb_dwc_hal_ep_char_t *ep_char)
{
    unsigned int interval = FRAME_LIST_LEN;     // Rounded down to a power of two, as the HAL schedules it
    while (interval > 1 && interval > ep_char->periodic.interval) {
        interval >>= 1;
    }
    for (unsigned int k = 0; k < interval; k++) {
        const unsigned int offset = (ep_char->periodic.offset + k) % interval;
        bool taken = false;
        for (unsigned int i = offset; i < FRAME_LIST_LEN && !taken; i += interval) {
            taken = (port->hal->periodic_frame_list[i] & port->ls_pre_periodic_chans) != 0;
        }
        if (!taken) {
            ep_char->periodic.offset = offset;
            return;
        }
    }
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
        if (pipe->ep_char.ls_via_fs_hub && pipe->ep_char.type == USB_DWC_XFER_TYPE_INTR &&
                chan_error == USB_DWC_HAL_CHAN_ERROR_XCS_XACT &&
                pipe->port->flags.conn_dev_ena && !pipe->cs_flags.waiting_halt && _ls_intr_retry(pipe)) {
            break;
        }
        pipe->last_event = pipe_decode_error_event(chan_error);"""

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

# Diagnostic, from NonaSuomy's USB_DWC_TT_DEBUG error detail: one line per channel error on
# a pipe to a low-speed device behind a full-speed hub, retried or not. HCINT is read before
# the HAL decodes (and clears) it; the stage's QTD word is read without the clearing parse.
_LS_DEBUG_PUBLIC_ANCHOR = (
    """esp_err_t usb_host_install(const usb_host_config_t *config);"""
)

_LS_DEBUG_PUBLIC_REPLACEMENT = """esp_err_t usb_host_install(const usb_host_config_t *config);

/**
 * @brief Log every channel error of a low-speed device behind a full-speed hub (diagnostic)
 */
extern bool usb_host_ls_hub_debug;"""

_LS_DEBUG_CHAN_ANCHOR = """static hcd_pipe_event_t _intr_hdlr_chan(pipe_t *pipe, usb_dwc_hal_chan_t *chan_obj, bool *yield)
{
    usb_dwc_hal_chan_event_t chan_event = usb_dwc_hal_chan_decode_intr(chan_obj);"""

_LS_DEBUG_CHAN_REPLACEMENT = """bool usb_host_ls_hub_debug = false;

static void _ls_hub_debug_log(pipe_t *pipe, usb_dwc_hal_chan_t *chan_obj, usb_dwc_hal_chan_error_t chan_error, uint32_t raw_chan_intrs)
{
    int stage = -1;
    uint32_t qtd = 0;
    if (pipe->ep_char.type == USB_DWC_XFER_TYPE_CTRL) {
        dma_buffer_block_t *buffer = pipe->buffers[pipe->multi_buffer_control.rd_idx];
        stage = buffer->flags.ctrl.cur_stg;
        CACHE_SYNC_XFER_DESCRIPTOR_LIST_M2C(buffer);
        qtd = ((usb_dwc_ll_dma_qtd_t *)buffer->xfer_desc_list)[stage].buffer_status_val;
    }
    ESP_EARLY_LOGE(HCD_DWC_TAG, "LS hub error: addr=%u ep=0x%02x type=%u stage=%d err=%d retry=%u frame=%u "
                   "HCINT=0x%08x HCCHAR=0x%08x HCTSIZ=0x%08x QTD=0x%08x",
                   (unsigned)pipe->ep_char.dev_addr, (unsigned)pipe->ep_char.bEndpointAddress, (unsigned)pipe->ep_char.type,
                   stage, (int)chan_error, (unsigned)pipe->ls_ctrl_retry_count,
                   (unsigned)usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal), (unsigned)raw_chan_intrs,
                   (unsigned)chan_obj->regs->hcchar_reg.val, (unsigned)chan_obj->regs->hctsiz_reg.val, (unsigned)qtd);
}

static hcd_pipe_event_t _intr_hdlr_chan(pipe_t *pipe, usb_dwc_hal_chan_t *chan_obj, bool *yield)
{
    const uint32_t raw_chan_intrs = (usb_host_ls_hub_debug && pipe->ep_char.ls_via_fs_hub) ? chan_obj->regs->hcint_reg.val : 0;
    usb_dwc_hal_chan_event_t chan_event = usb_dwc_hal_chan_decode_intr(chan_obj);"""

_LS_DEBUG_ERROR_ANCHOR = """        usb_dwc_hal_chan_error_t chan_error = usb_dwc_hal_chan_get_error(chan_obj);
        if (pipe->ep_char.ls_via_fs_hub && pipe->ep_char.type == USB_DWC_XFER_TYPE_CTRL &&"""

_LS_DEBUG_ERROR_REPLACEMENT = """        usb_dwc_hal_chan_error_t chan_error = usb_dwc_hal_chan_get_error(chan_obj);
        if (usb_host_ls_hub_debug && pipe->ep_char.ls_via_fs_hub) {
            _ls_hub_debug_log(pipe, chan_obj, chan_error, raw_chan_intrs);
        }
        if (pipe->ep_char.ls_via_fs_hub && pipe->ep_char.type == USB_DWC_XFER_TYPE_CTRL &&"""

# The diagnostic printed from the interrupt with ESP_EARLY_LOGE, which goes straight to the ROM
# console (esp_rom_printf) and never through the vprintf hook ESPHome's logger installs, so it
# was invisible in ESPHome's log. It now records into a ring the application drains and logs,
# plus a record when a pipe to a low-speed device behind a hub opens with PREamble set.
_LS_DEBUG_RING_PUBLIC_ANCHOR = """extern bool usb_host_ls_hub_debug;"""

_LS_DEBUG_RING_PUBLIC_REPLACEMENT = """extern bool usb_host_ls_hub_debug;

/**
 * @brief One record of that diagnostic, read by the application
 */
typedef struct {
    uint8_t kind;       /**< 0: pipe opened with PREamble, 1: channel error */
    uint8_t dev_addr;
    uint8_t ep;
    uint8_t xfer_type;
    int8_t stage;       /**< Control stage, -1 if not a control transfer */
    uint8_t err;        /**< usb_dwc_hal_chan_error_t */
    uint8_t retry;
    uint8_t mps;
    uint32_t frame;
    uint32_t hcint;     /**< Read before the HAL clears it */
    uint32_t hcchar;
    uint32_t hctsiz;
    uint32_t qtd;       /**< The stage's descriptor word */
} usb_host_ls_hub_debug_entry_t;

#define USB_HOST_LS_HUB_DEBUG_ENTRIES 16
extern usb_host_ls_hub_debug_entry_t usb_host_ls_hub_debug_entries[USB_HOST_LS_HUB_DEBUG_ENTRIES];
/** Records written so far; entry n is at n % USB_HOST_LS_HUB_DEBUG_ENTRIES. */
extern volatile uint32_t usb_host_ls_hub_debug_count;"""

_LS_DEBUG_RING_DEF_ANCHOR = """bool usb_host_ls_hub_debug = false;

static void _ls_hub_debug_log("""

_LS_DEBUG_RING_DEF_REPLACEMENT = """#include "usb/usb_host.h"

bool usb_host_ls_hub_debug = false;
usb_host_ls_hub_debug_entry_t usb_host_ls_hub_debug_entries[USB_HOST_LS_HUB_DEBUG_ENTRIES];
volatile uint32_t usb_host_ls_hub_debug_count = 0;
static uint32_t s_ls_hub_debug_reserved = 0;

static void _ls_hub_debug_record(const usb_host_ls_hub_debug_entry_t *entry)
{
    const uint32_t index = __atomic_fetch_add(&s_ls_hub_debug_reserved, 1, __ATOMIC_RELAXED);
    usb_host_ls_hub_debug_entries[index % USB_HOST_LS_HUB_DEBUG_ENTRIES] = *entry;
    __atomic_fetch_add(&usb_host_ls_hub_debug_count, 1, __ATOMIC_RELEASE);
}

static void _ls_hub_debug_log("""

_LS_DEBUG_RING_LOG_ANCHOR = """    ESP_EARLY_LOGE(HCD_DWC_TAG, "LS hub error: addr=%u ep=0x%02x type=%u stage=%d err=%d retry=%u frame=%u "
                   "HCINT=0x%08x HCCHAR=0x%08x HCTSIZ=0x%08x QTD=0x%08x",
                   (unsigned)pipe->ep_char.dev_addr, (unsigned)pipe->ep_char.bEndpointAddress, (unsigned)pipe->ep_char.type,
                   stage, (int)chan_error, (unsigned)pipe->ls_ctrl_retry_count,
                   (unsigned)usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal), (unsigned)raw_chan_intrs,
                   (unsigned)chan_obj->regs->hcchar_reg.val, (unsigned)chan_obj->regs->hctsiz_reg.val, (unsigned)qtd);"""

_LS_DEBUG_RING_LOG_REPLACEMENT = """    const usb_host_ls_hub_debug_entry_t entry = {
        .kind = 1,
        .dev_addr = (uint8_t)pipe->ep_char.dev_addr,
        .ep = (uint8_t)pipe->ep_char.bEndpointAddress,
        .xfer_type = (uint8_t)pipe->ep_char.type,
        .stage = (int8_t)stage,
        .err = (uint8_t)chan_error,
        .retry = pipe->ls_ctrl_retry_count,
        .mps = (uint8_t)(pipe->ep_char.mps > 255 ? 255 : pipe->ep_char.mps),
        .frame = usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal),
        .hcint = raw_chan_intrs,
        .hcchar = chan_obj->regs->hcchar_reg.val,
        .hctsiz = chan_obj->regs->hctsiz_reg.val,
        .qtd = qtd,
    };
    _ls_hub_debug_record(&entry);"""

_LS_DEBUG_RING_ALLOC_ANCHOR = """    pipe->ls_first_launch_pending = pipe->ep_char.ls_via_fs_hub;
    pipe->state = HCD_PIPE_STATE_ACTIVE;"""

_LS_DEBUG_RING_ALLOC_REPLACEMENT = """    pipe->ls_first_launch_pending = pipe->ep_char.ls_via_fs_hub;
    if (usb_host_ls_hub_debug && pipe->ep_char.ls_via_fs_hub) {
        const usb_host_ls_hub_debug_entry_t entry = {
            .kind = 0,
            .dev_addr = (uint8_t)pipe->ep_char.dev_addr,
            .ep = (uint8_t)pipe->ep_char.bEndpointAddress,
            .xfer_type = (uint8_t)pipe->ep_char.type,
            .stage = -1,
            .mps = (uint8_t)(pipe->ep_char.mps > 255 ? 255 : pipe->ep_char.mps),
        };
        _ls_hub_debug_record(&entry);
    }
    pipe->state = HCD_PIPE_STATE_ACTIVE;"""

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

# Low-speed behind a full-speed hub: the frames the periodic (interrupt) pipes of such devices
# are scheduled in carry a PREamble transaction already, so a control stage of such a device
# must not start in one of them or in the frame before one (a NAKed stage runs on into the next
# frame), and two such pipes get frames of their own. The port keeps which channels those pipes
# use; the frame list says which frames they poll in.
_LS_PERIODIC_PORT_FIELD_ANCHOR = """    usb_dwc_hal_context_t *hal;
    void *frame_list;"""

_LS_PERIODIC_PORT_FIELD_REPLACEMENT = """    usb_dwc_hal_context_t *hal;
    void *frame_list;
    uint32_t ls_pre_periodic_chans;     // Channels of periodic pipes with PREamble (LS via FS hub)"""

_LS_PERIODIC_WAIT_ANCHOR = """    for (int waited_us = 0; waited_us < 3000; waited_us += 10) {
        if (usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal) != start_frame) {
            return;
        }"""

_LS_PERIODIC_WAIT_REPLACEMENT = """    const uint32_t busy_chans = (pipe->ep_char.type == USB_DWC_XFER_TYPE_CTRL) ? pipe->port->ls_pre_periodic_chans : 0;
    for (int waited_us = 0; waited_us < 3000; waited_us += 10) {
        const uint32_t frame = usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal);
        if (frame != start_frame &&
                !(pipe->port->hal->periodic_frame_list[frame % FRAME_LIST_LEN] & busy_chans) &&
                !(pipe->port->hal->periodic_frame_list[(frame + 1) % FRAME_LIST_LEN] & busy_chans)) {
            return;
        }"""

_LS_PERIODIC_EXEC_ANCHOR = """    pipe->ls_ctrl_retry_count = 0;
    if (pipe->ls_first_launch_pending) {"""

_LS_PERIODIC_EXEC_REPLACEMENT = """    pipe->ls_ctrl_retry_count = 0;
    if (pipe->ls_first_launch_pending ||
            (pipe->ep_char.ls_via_fs_hub && pipe->ep_char.type == USB_DWC_XFER_TYPE_CTRL &&
             pipe->port->ls_pre_periodic_chans)) {"""

_LS_PERIODIC_ALLOC_ANCHOR = """    usb_dwc_hal_chan_set_ep_char(port->hal, pipe->chan_obj, &pipe->ep_char);
    CACHE_SYNC_FRAME_LIST(port->frame_list);"""

_LS_PERIODIC_ALLOC_REPLACEMENT = """    if (pipe->ep_char.ls_via_fs_hub && pipe->ep_char.type == USB_DWC_XFER_TYPE_INTR) {
        _ls_periodic_set_offset(port, &pipe->ep_char);
    }
    usb_dwc_hal_chan_set_ep_char(port->hal, pipe->chan_obj, &pipe->ep_char);
    if (pipe->ep_char.ls_via_fs_hub &&
            (pipe->ep_char.type == USB_DWC_XFER_TYPE_INTR || pipe->ep_char.type == USB_DWC_XFER_TYPE_ISOCHRONOUS)) {
        port->ls_pre_periodic_chans |= 1U << pipe->chan_obj->flags.chan_idx;
    }
    CACHE_SYNC_FRAME_LIST(port->frame_list);"""

_LS_PERIODIC_FREE_ANCHOR = """    pipe->port->num_pipes_idle--;
    usb_dwc_hal_chan_free(pipe->port->hal, pipe->chan_obj);"""

_LS_PERIODIC_FREE_REPLACEMENT = """    pipe->port->num_pipes_idle--;
    pipe->port->ls_pre_periodic_chans &= ~(1U << pipe->chan_obj->flags.chan_idx);
    usb_dwc_hal_chan_free(pipe->port->hal, pipe->chan_obj);"""

# Data stage of a low-speed control transfer behind a full-speed hub, one packet per frame (see
# _ls_ctrl_data_next() in _LS_RETRY_HELPER_REPLACEMENT).
_LS_SPLIT_FLAGS_ANCHOR = """            uint32_t cur_stg: 2;            // Index of the current stage (e.g., 0 is setup stage, 2 is status stage)
            uint32_t reserved28: 28;"""

_LS_SPLIT_FLAGS_REPLACEMENT = """            uint32_t cur_stg: 2;            // Index of the current stage (e.g., 0 is setup stage, 2 is status stage)
            uint32_t ls_split: 1;           // Data stage run one packet at a time (LS via FS hub)
            uint32_t ls_done: 16;           // Data stage bytes done so far, with ls_split
            uint32_t reserved11: 11;"""

_LS_SPLIT_FILL_ANCHOR = """    buffer->flags.ctrl.data_stg_skip = data_stg_skip;
    buffer->flags.ctrl.cur_stg = 0;"""

_LS_SPLIT_FILL_REPLACEMENT = """    buffer->flags.ctrl.data_stg_skip = data_stg_skip;
    buffer->flags.ctrl.cur_stg = 0;
    buffer->flags.ctrl.ls_split = 0;
    buffer->flags.ctrl.ls_done = 0;"""

_LS_SPLIT_CONT_ANCHOR = """    bool next_dir_is_in;
    int next_pid;
    assert(buffer_inflight->flags.ctrl.cur_stg != 2);"""

_LS_SPLIT_CONT_REPLACEMENT = """    bool next_dir_is_in;
    int next_pid;
    assert(buffer_inflight->flags.ctrl.cur_stg != 2);
    if (buffer_inflight->flags.ctrl.cur_stg == 1 && buffer_inflight->flags.ctrl.ls_split &&
            _ls_ctrl_data_next(pipe, buffer_inflight)) {
        return;
    }"""

_LS_SPLIT_DATA_ANCHOR = """            next_pid = 1;   // Data stage always starts with a PID of DATA1
            buffer_inflight->flags.ctrl.cur_stg = 1;"""

_LS_SPLIT_DATA_REPLACEMENT = """            next_pid = 1;   // Data stage always starts with a PID of DATA1
            buffer_inflight->flags.ctrl.cur_stg = 1;
            if (pipe->ep_char.ls_via_fs_hub) {
                _ls_ctrl_data_start(pipe, buffer_inflight);
            }"""

_LS_SPLIT_PARSE_ANCHOR = """    } else {
        // Parse the data stage for the remaining length
        int rem_len;"""

_LS_SPLIT_PARSE_REPLACEMENT = """    } else if (buffer->flags.ctrl.ls_split) {
        // Data stage run one packet at a time: its bytes were counted as they came
        transfer->actual_num_bytes = sizeof(usb_setup_packet_t) + buffer->flags.ctrl.ls_done;
    } else {
        // Parse the data stage for the remaining length
        int rem_len;"""

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
    ("include/usb/usb_host.h", _LS_DEBUG_PUBLIC_ANCHOR, _LS_DEBUG_PUBLIC_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_DEBUG_CHAN_ANCHOR, _LS_DEBUG_CHAN_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_DEBUG_ERROR_ANCHOR, _LS_DEBUG_ERROR_REPLACEMENT),
    (
        "include/usb/usb_host.h",
        _LS_DEBUG_RING_PUBLIC_ANCHOR,
        _LS_DEBUG_RING_PUBLIC_REPLACEMENT,
    ),
    ("src/hcd_dwc.c", _LS_DEBUG_RING_DEF_ANCHOR, _LS_DEBUG_RING_DEF_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_DEBUG_RING_LOG_ANCHOR, _LS_DEBUG_RING_LOG_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_DEBUG_RING_ALLOC_ANCHOR, _LS_DEBUG_RING_ALLOC_REPLACEMENT),
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
    ("src/hcd_dwc.c", _LS_PERIODIC_PORT_FIELD_ANCHOR, _LS_PERIODIC_PORT_FIELD_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PERIODIC_WAIT_ANCHOR, _LS_PERIODIC_WAIT_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PERIODIC_EXEC_ANCHOR, _LS_PERIODIC_EXEC_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PERIODIC_ALLOC_ANCHOR, _LS_PERIODIC_ALLOC_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PERIODIC_FREE_ANCHOR, _LS_PERIODIC_FREE_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_SPLIT_FLAGS_ANCHOR, _LS_SPLIT_FLAGS_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_SPLIT_FILL_ANCHOR, _LS_SPLIT_FILL_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_SPLIT_CONT_ANCHOR, _LS_SPLIT_CONT_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_SPLIT_DATA_ANCHOR, _LS_SPLIT_DATA_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_SPLIT_PARSE_ANCHOR, _LS_SPLIT_PARSE_REPLACEMENT),
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
